#pragma once

#include <Core/BlockMissingValues.h>

namespace DB
{
struct ColumnWithTypeAndName;

bool insertNullAsDefaultIfNeeded(ColumnWithTypeAndName & input_column, const ColumnWithTypeAndName & header_column, size_t column_i, BlockMissingValues * block_missing_values);

}
