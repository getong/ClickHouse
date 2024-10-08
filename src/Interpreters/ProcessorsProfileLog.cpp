#include <Interpreters/ProcessorsProfileLog.h>

#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <base/getFQDNOrHostName.h>
#include <Common/ClickHouseRevision.h>
#include <Common/DateLUTImpl.h>
#include <Common/logger_useful.h>

#include <array>

namespace DB
{

ColumnsDescription ProcessorProfileLogElement::getColumnsDescription()
{
    return ColumnsDescription
    {
        {"hostname", std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>()), "Hostname of the server executing the query."},
        {"event_date", std::make_shared<DataTypeDate>(), "The date when the event happened."},
        {"event_time", std::make_shared<DataTypeDateTime>(), "The date and time when the event happened."},
        {"event_time_microseconds", std::make_shared<DataTypeDateTime64>(6), "The date and time with microseconds precision when the event happened."},

        {"id", std::make_shared<DataTypeUInt64>(), "ID of processor."},
        {"parent_ids", std::make_shared<DataTypeArray>(std::make_shared<DataTypeUInt64>()), "Parent processors IDs."},
        {"plan_step", std::make_shared<DataTypeUInt64>(), "ID of the query plan step which created this processor. The value is zero if the processor was not added from any step."},
        {"plan_step_name", std::make_shared<DataTypeString>(), "Name of the query plan step which created this processor. The value is empty if the processor was not added from any step."},
        {"plan_step_description", std::make_shared<DataTypeString>(), "Description of the query plan step which created this processor. The value is empty if the processor was not added from any step."},
        {"plan_group", std::make_shared<DataTypeUInt64>(), "Group of the processor if it was created by query plan step. A group is a logical partitioning of processors added from the same query plan step. Group is used only for beautifying the result of EXPLAIN PIPELINE result."},

        {"initial_query_id", std::make_shared<DataTypeString>(), "ID of the initial query (for distributed query execution)."},
        {"query_id", std::make_shared<DataTypeString>(), "ID of the query."},
        {"name", std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>()), "Name of the processor."},
        {"elapsed_us", std::make_shared<DataTypeUInt64>(), "Number of microseconds this processor was executed."},
        {"input_wait_elapsed_us", std::make_shared<DataTypeUInt64>(), "Number of microseconds this processor was waiting for data (from other processor)."},
        {"output_wait_elapsed_us", std::make_shared<DataTypeUInt64>(), "Number of microseconds this processor was waiting because output port was full."},
        {"input_rows", std::make_shared<DataTypeUInt64>(), "The number of rows consumed by processor."},
        {"input_bytes", std::make_shared<DataTypeUInt64>(), "The number of bytes consumed by processor."},
        {"output_rows", std::make_shared<DataTypeUInt64>(), "The number of rows generated by processor."},
        {"output_bytes", std::make_shared<DataTypeUInt64>(), "The number of bytes generated by processor."},
    };
}

void ProcessorProfileLogElement::appendToBlock(MutableColumns & columns) const
{
    size_t i = 0;

    columns[i++]->insert(getFQDNOrHostName());
    columns[i++]->insert(DateLUT::instance().toDayNum(event_time).toUnderType());
    columns[i++]->insert(event_time);
    columns[i++]->insert(event_time_microseconds);

    columns[i++]->insert(id);
    {
        Array parent_ids_array;
        parent_ids_array.reserve(parent_ids.size());
        for (const UInt64 parent : parent_ids)
            parent_ids_array.emplace_back(parent);
        columns[i++]->insert(parent_ids_array);
    }

    columns[i++]->insert(plan_step);
    columns[i++]->insert(plan_step_name);
    columns[i++]->insert(plan_step_description);
    columns[i++]->insert(plan_group);
    columns[i++]->insertData(initial_query_id.data(), initial_query_id.size());
    columns[i++]->insertData(query_id.data(), query_id.size());
    columns[i++]->insertData(processor_name.data(), processor_name.size());
    columns[i++]->insert(elapsed_us);
    columns[i++]->insert(input_wait_elapsed_us);
    columns[i++]->insert(output_wait_elapsed_us);
    columns[i++]->insert(input_rows);
    columns[i++]->insert(input_bytes);
    columns[i++]->insert(output_rows);
    columns[i++]->insert(output_bytes);
}


}
