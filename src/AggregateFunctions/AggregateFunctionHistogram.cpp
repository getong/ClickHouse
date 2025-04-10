#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <AggregateFunctions/FactoryHelpers.h>
#include <AggregateFunctions/Helpers.h>
#include <Common/FieldVisitorConvertToNumber.h>

#include <Common/NaNUtils.h>

#include <Columns/ColumnVector.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnArray.h>
#include <Common/assert_cast.h>

#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeTuple.h>

#include <IO/WriteBuffer.h>
#include <IO/ReadBuffer.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <IO/VarInt.h>

#include <AggregateFunctions/IAggregateFunction.h>

#include <queue>
#include <cmath>
#include <cstddef>


namespace DB
{
struct Settings;

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int BAD_ARGUMENTS;
    extern const int UNSUPPORTED_PARAMETER;
    extern const int PARAMETER_OUT_OF_BOUND;
    extern const int TOO_LARGE_ARRAY_SIZE;
    extern const int INCORRECT_DATA;
}


namespace
{

/** distance compression algorithm implementation
  * http://jmlr.org/papers/volume11/ben-haim10a/ben-haim10a.pdf
  */
class AggregateFunctionHistogramData
{
public:
    using Mean = Float64;
    using Weight = Float64;

    constexpr static size_t bins_count_limit = 250;

private:
    struct WeightedValue
    {
        Mean mean;
        Weight weight;

        WeightedValue operator+(const WeightedValue & other) const
        {
            return {mean + other.weight * (other.mean - mean) / (other.weight + weight), other.weight + weight};
        }
    };

    // quantity of stored weighted-values
    UInt32 size;

    // calculated lower and upper bounds of seen points
    Mean lower_bound;
    Mean upper_bound;

    // Weighted values representation of histogram.
    WeightedValue points[0];

    void sort()
    {
        ::sort(points, points + size,
            [](const WeightedValue & first, const WeightedValue & second)
            {
                return first.mean < second.mean;
            });
    }

    template <typename T>
    struct PriorityQueueStorage
    {
        size_t size = 0;
        T * data_ptr;

        explicit PriorityQueueStorage(T * value)
            : data_ptr(value)
        {
        }

        void push_back(T val) /// NOLINT
        {
            data_ptr[size] = std::move(val);
            ++size;
        }

        void pop_back() { --size; } /// NOLINT
        T * begin() { return data_ptr; }
        T * end() const { return data_ptr + size; }
        bool empty() const { return size == 0; }
        T & front() { return *data_ptr; }
        const T & front() const { return *data_ptr; }

        using value_type = T;
        using reference = T&;
        using const_reference = const T&;
        using size_type = size_t;
    };

    /**
     * Repeatedly fuse most close values until max_bins bins left
     */
    void compress(UInt32 max_bins)
    {
        sort();
        auto new_size = size;
        if (size <= max_bins)
            return;

        // Maintain doubly-linked list of "active" points
        // and store neighbour pairs in priority queue by distance
        std::vector<UInt32> previous(size + 1);
        std::vector<UInt32> next(size + 1);
        std::vector<bool> active(size + 1, true);
        active[size] = false;

        auto delete_node = [&](UInt32 i)
        {
            previous[next[i]] = previous[i];
            next[previous[i]] = next[i];
            active[i] = false;
        };

        for (size_t i = 0; i <= size; ++i)
        {
            previous[i] = static_cast<UInt32>(i - 1);
            next[i] = static_cast<UInt32>(i + 1);
        }

        next[size] = 0;
        previous[0] = size;

        using QueueItem = std::pair<Mean, UInt32>;

        std::vector<QueueItem> storage(2 * size - max_bins);

        std::priority_queue<QueueItem, PriorityQueueStorage<QueueItem>, std::greater<>> queue{
            std::greater<>(), PriorityQueueStorage<QueueItem>(storage.data())};

        auto quality = [&](UInt32 i) { return points[next[i]].mean - points[i].mean; };

        for (size_t i = 0; i + 1 < size; ++i)
            queue.push({quality(static_cast<UInt32>(i)), i});

        while (new_size > max_bins && !queue.empty())
        {
            auto min_item = queue.top();
            queue.pop();
            auto left = min_item.second;
            auto right = next[left];

            if (!active[left] || !active[right] || quality(left) > min_item.first)
                continue;

            points[left] = points[left] + points[right];

            delete_node(right);
            if (active[next[left]])
                queue.push({quality(left), left});
            if (active[previous[left]])
                queue.push({quality(previous[left]), previous[left]});

            --new_size;
        }

        size_t left = 0;
        for (size_t right = 0; right < size; ++right)
        {
            if (active[right])
            {
                points[left] = points[right];
                ++left;
            }
        }
        size = new_size;
    }

    /***
     * Delete too close points from histogram.
     * Assumes that points are sorted.
     */
    void unique()
    {
        if (size == 0)
            return;

        size_t left = 0;

        for (auto right = left + 1; right < size; ++right)
        {
            // Fuse points if their text representations differ only in last digit
            auto min_diff = 10 * (points[left].mean + points[right].mean) * std::numeric_limits<Mean>::epsilon();
            if (points[left].mean + std::fabs(min_diff) >= points[right].mean)
            {
                points[left] = points[left] + points[right];
            }
            else
            {
                ++left;
                points[left] = points[right];
            }
        }
        size = static_cast<UInt32>(left + 1);
    }

public:
    AggregateFunctionHistogramData()
        : size(0)
        , lower_bound(std::numeric_limits<Mean>::max())
        , upper_bound(std::numeric_limits<Mean>::lowest())
    {
        static_assert(offsetof(AggregateFunctionHistogramData, points) == sizeof(AggregateFunctionHistogramData), "points should be last member");
    }

    static size_t structSize(size_t max_bins)
    {
        return sizeof(AggregateFunctionHistogramData) + max_bins * 2 * sizeof(WeightedValue);
    }

    void insertResultInto(ColumnVector<Mean> & to_lower, ColumnVector<Mean> & to_upper, ColumnVector<Weight> & to_weights, UInt32 max_bins)
    {
        compress(max_bins);
        unique();

        for (size_t i = 0; i < size; ++i)
        {
            to_lower.insertValue((i == 0) ? lower_bound : (points[i].mean + points[i - 1].mean) / 2);
            to_upper.insertValue((i + 1 == size) ? upper_bound : (points[i].mean + points[i + 1].mean) / 2);

            // linear density approximation
            Weight lower_weight = (i == 0) ? points[i].weight : ((points[i - 1].weight) + points[i].weight * 3) / 4;
            Weight upper_weight = (i + 1 == size) ? points[i].weight : (points[i + 1].weight + points[i].weight * 3) / 4;
            to_weights.insertValue((lower_weight + upper_weight) / 2);
        }
    }

    void add(Mean value, Weight weight, UInt32 max_bins)
    {
        // nans break sort and compression
        // infs don't fit in bins partition method
        if (!isFinite(value))
            throw Exception(ErrorCodes::INCORRECT_DATA, "Invalid value (inf or nan) for aggregation by 'histogram' function");

        points[size] = {value, weight};
        ++size;
        lower_bound = std::min(lower_bound, value);
        upper_bound = std::max(upper_bound, value);

        if (size >= max_bins * 2)
            compress(max_bins);
    }

    void merge(const AggregateFunctionHistogramData & other, UInt32 max_bins)
    {
        lower_bound = std::min(lower_bound, other.lower_bound);
        upper_bound = std::max(upper_bound, other.upper_bound);
        for (size_t i = 0; i < other.size; ++i)
            add(other.points[i].mean, other.points[i].weight, max_bins);
    }

    void write(WriteBuffer & buf) const
    {
        writeBinary(lower_bound, buf);
        writeBinary(upper_bound, buf);

        writeVarUInt(size, buf);
        buf.write(reinterpret_cast<const char *>(points), size * sizeof(WeightedValue));
    }

    void read(ReadBuffer & buf, UInt32 max_bins)
    {
        readBinary(lower_bound, buf);
        readBinary(upper_bound, buf);

        readVarUInt(size, buf);
        if (size > max_bins * 2)
            throw Exception(ErrorCodes::TOO_LARGE_ARRAY_SIZE, "Too many bins");
        static constexpr size_t max_size = 1_GiB;
        if (size > max_size)
            throw Exception(ErrorCodes::TOO_LARGE_ARRAY_SIZE,
                            "Too large array size in histogram (maximum: {})", max_size);

        buf.readStrict(reinterpret_cast<char *>(points), size * sizeof(WeightedValue));
    }
};

template <typename T>
class AggregateFunctionHistogram final: public IAggregateFunctionDataHelper<AggregateFunctionHistogramData, AggregateFunctionHistogram<T>>
{
private:
    using Data = AggregateFunctionHistogramData;

    const UInt32 max_bins;

public:
    AggregateFunctionHistogram(UInt32 max_bins_, const DataTypes & arguments, const Array & params)
        : IAggregateFunctionDataHelper<AggregateFunctionHistogramData, AggregateFunctionHistogram<T>>(arguments, params, createResultType())
        , max_bins(max_bins_)
    {
    }

    size_t sizeOfData() const override
    {
        return Data::structSize(max_bins);
    }
    static DataTypePtr createResultType()
    {
        DataTypes types;
        auto mean = std::make_shared<DataTypeNumber<Data::Mean>>();
        auto weight = std::make_shared<DataTypeNumber<Data::Weight>>();

        // lower bound
        types.emplace_back(mean);
        // upper bound
        types.emplace_back(mean);
        // weight
        types.emplace_back(weight);

        auto tuple = std::make_shared<DataTypeTuple>(types);
        return std::make_shared<DataTypeArray>(tuple);
    }

    bool allocatesMemoryInArena() const override { return false; }

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        auto val = assert_cast<const ColumnVector<T> &>(*columns[0]).getData()[row_num];
        this->data(place).add(static_cast<Data::Mean>(val), 1, max_bins);
    }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        this->data(place).merge(this->data(rhs), max_bins);
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf, std::optional<size_t> /* version */) const override
    {
        this->data(place).write(buf);
    }

    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, std::optional<size_t> /* version */, Arena *) const override
    {
        this->data(place).read(buf, max_bins);
    }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        auto & data = this->data(place);

        auto & to_array = assert_cast<ColumnArray &>(to);
        ColumnArray::Offsets & offsets_to = to_array.getOffsets();
        auto & to_tuple = assert_cast<ColumnTuple &>(to_array.getData());

        auto & to_lower = assert_cast<ColumnVector<Data::Mean> &>(to_tuple.getColumn(0));
        auto & to_upper = assert_cast<ColumnVector<Data::Mean> &>(to_tuple.getColumn(1));
        auto & to_weights = assert_cast<ColumnVector<Data::Weight> &>(to_tuple.getColumn(2));
        data.insertResultInto(to_lower, to_upper, to_weights, max_bins);

        offsets_to.push_back(to_tuple.size());
    }

    String getName() const override { return "histogram"; }
};


AggregateFunctionPtr createAggregateFunctionHistogram(const std::string & name, const DataTypes & arguments, const Array & params, const Settings *)
{
    if (params.size() != 1)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Function {} requires single parameter: bins count", name);

    if (params[0].getType() != Field::Types::UInt64)
        throw Exception(ErrorCodes::UNSUPPORTED_PARAMETER, "Invalid type for bins count");

    UInt32 bins_count = applyVisitor(FieldVisitorConvertToNumber<UInt32>(), params[0]);

    auto limit = AggregateFunctionHistogramData::bins_count_limit;
    if (bins_count > limit)
        throw Exception(ErrorCodes::PARAMETER_OUT_OF_BOUND, "Unsupported bins count. Should not be greater than {}", limit);

    if (bins_count == 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Bin count should be positive");

    assertUnary(name, arguments);
    AggregateFunctionPtr res(createWithNumericType<AggregateFunctionHistogram>(*arguments[0], bins_count, arguments, params));

    if (!res)
        throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                        "Illegal type {} of argument for aggregate function {}", arguments[0]->getName(), name);

    return res;
}

}

void registerAggregateFunctionHistogram(AggregateFunctionFactory & factory)
{
    factory.registerFunction("histogram", createAggregateFunctionHistogram);
}

}
