#pragma once

#include <type_traits>
#include <AggregateFunctions/AggregateFunctionAvg.h>

namespace DB
{
template <class T>
using FieldType = std::conditional_t<IsDecimalNumber<T>,
    std::conditional_t<std::is_same_v<T, Decimal256>, Decimal256, Decimal128>,
    std::conditional_t<DecimalOrExtendedInt<T>,
        Float64, // no way to do UInt128 * UInt128, better cast to Float64
        NearestFieldType<T>>>;

template <class T, class U>
using MaxFieldType = std::conditional_t<(sizeof(FieldType<T>) > sizeof(FieldType<U>)),
    FieldType<T>, FieldType<U>>;

template <class Value, class Weight>
class AggregateFunctionAvgWeighted final :
    public AggregateFunctionAvgBase<
        MaxFieldType<Value, Weight>, FieldType<Weight>, AggregateFunctionAvgWeighted<Value, Weight>>
{
public:
    using Base = AggregateFunctionAvgBase<
        MaxFieldType<Value, Weight>, FieldType<Weight>, AggregateFunctionAvgWeighted<Value, Weight>>;
    using Base::Base;

    using ValueT = MaxFieldType<Value, Weight>;

    void add(AggregateDataPtr place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        const Value value = static_cast<const DecimalOrVectorCol<Value> &>(*columns[0]).getData()[row_num];
        const Weight weight = static_cast<const DecimalOrVectorCol<Weight> &>(*columns[1]).getData()[row_num];

        this->data(place).numerator += static_cast<ValueT>(value) * static_cast<ValueT>(weight);

        this->data(place).denominator += static_cast<FieldType<Weight>>(weight);
    }

    String getName() const override { return "avgWeighted"; }
};
}
