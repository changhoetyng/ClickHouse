#include <Functions/IFunction.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/DateTimeTransforms.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDate32.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypesNumber.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnDecimal.h>
#include <Columns/ColumnsDateTime.h>
#include <Columns/ColumnsNumber.h>
#include <Interpreters/castColumn.h>

#include <Common/DateLUT.h>
#include <Common/typeid_cast.h>

#include <array>
#include <cmath>

namespace DB
{
namespace ErrorCodes
{
    extern const int VALUE_IS_OUT_OF_RANGE_OF_DATA_TYPE;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int ARGUMENT_OUT_OF_BOUND;
}

namespace
{

struct DateTraits
{
    static constexpr auto name = "fromDaysSinceYearZero";
    using ReturnDataType = DataTypeDate;

    static constexpr auto MIN_DAYS = 719528;
    static constexpr auto MAX_DAYS = 785063;
};

struct DateTraits32
{
    static constexpr auto name = "fromDaysSinceYearZero32";
    using ReturnDataType = DataTypeDate32;

    static constexpr auto MIN_DAYS = 693961;
    static constexpr auto MAX_DAYS = 840056;
};

template <typename Traits>
class FunctionFromDaysSinceYearZero : public IFunction
{

public:
    static constexpr auto name = Traits::name;
    using RawReturnType = typename Traits::ReturnDataType::FieldType;

    static FunctionPtr create(ContextPtr ctx) { return std::make_shared<FunctionFromDaysSinceYearZero>(ctx); }

    explicit FunctionFromDaysSinceYearZero(ContextPtr ctx_) : ctx(ctx_) {}

    String getName() const override { return name; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }
    bool useDefaultImplementationForConstants() const override { return true; }
    size_t getNumberOfArguments() const override { return 1; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        FunctionArgumentDescriptors args{
            {"days", &isNativeUnsignedInteger<IDataType>, nullptr, "UInt*"}
        };

        validateFunctionArgumentTypes(*this, arguments, args);

        return std::make_shared<typename Traits::ReturnDataType>();
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const override
    {
        using namespace std::chrono;

        auto res_column = Traits::ReturnDataType::ColumnType::create(input_rows_count);
        const auto & src_column = arguments[0];

        [[maybe_unused]] FormatSettings::DateTimeOverflowBehavior date_time_overflow_behavior = default_date_time_overflow_behavior;

        if (ctx)
            date_time_overflow_behavior = ctx->getSettingsRef().date_time_overflow_behavior.value;

        auto try_type = [&]<typename T>(T)
        {
            using ColVecType = ColumnVector<T>;

            if (const ColVecType * col_vec = checkAndGetColumn<ColVecType>(src_column.column.get()))
            {
                switch (date_time_overflow_behavior)
                {
                    case FormatSettings::DateTimeOverflowBehavior::Throw:
                        execute<FormatSettings::DateTimeOverflowBehavior::Throw, T>(*col_vec, *res_column, input_rows_count);
                        break;
                    case FormatSettings::DateTimeOverflowBehavior::Ignore:
                        execute<FormatSettings::DateTimeOverflowBehavior::Ignore, T>(*col_vec, *res_column, input_rows_count);
                        break;
                    case FormatSettings::DateTimeOverflowBehavior::Saturate:
                        execute<FormatSettings::DateTimeOverflowBehavior::Saturate, T>(*col_vec, *res_column, input_rows_count);
                        break;
                }
                return true;
            }
            return false;
        };

        const auto res = false // NOLINT
            || try_type(UInt8{})
            || try_type(UInt16{})
            || try_type(UInt32{})
            || try_type(UInt64{})
            || try_type(Int8{})
            || try_type(Int16{})
            || try_type(Int32{})
            || try_type(Int64{});
        if (res)
            return res_column;

        throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "Illegal column while execute function {}", getName());
    }

    template <FormatSettings::DateTimeOverflowBehavior overflow_behaviour, typename T, typename ColVecType, typename ResCol>
    void execute(const ColVecType & col, ResCol & result_column, size_t rows_count) const
    {
        const auto & src_data = col.getData();
        auto & dst_data = result_column.getData();
        dst_data.resize(rows_count);

        using equivalent_integer = typename std::conditional_t<sizeof(T) == 4, UInt32, UInt64>;

        for (size_t i = 0; i < rows_count; ++i)
        {
            auto raw_value = src_data[i];
            if constexpr (std::is_signed_v<T>)
            {
                if (raw_value < 0) [[unlikely]]
                    throw Exception(ErrorCodes::ARGUMENT_OUT_OF_BOUND, "Negative value {} while executing function {}", static_cast<Int64>(raw_value), getName());
            }
            auto value = static_cast<equivalent_integer>(raw_value);
            if constexpr (overflow_behaviour == FormatSettings::DateTimeOverflowBehavior::Saturate)
            {
                if (value < Traits::MIN_DAYS)
                    value = Traits::MIN_DAYS;
                else if (value > Traits::MAX_DAYS)
                    value = Traits::MAX_DAYS;
                dst_data[i] = static_cast<RawReturnType>(value - ToDaysSinceYearZeroImpl::DAYS_BETWEEN_YEARS_0_AND_1970);
            }
            else if constexpr (overflow_behaviour == FormatSettings::DateTimeOverflowBehavior::Throw)
            {
                if (value < Traits::MIN_DAYS || value > Traits::MAX_DAYS) [[unlikely]]
                    throw Exception(ErrorCodes::VALUE_IS_OUT_OF_RANGE_OF_DATA_TYPE, "Value {} is out of bounds while executing function {}", static_cast<Int64>(value), getName());
                dst_data[i] = static_cast<RawReturnType>(value - ToDaysSinceYearZeroImpl::DAYS_BETWEEN_YEARS_0_AND_1970);
            } else
                dst_data[i] = static_cast<RawReturnType>(value - ToDaysSinceYearZeroImpl::DAYS_BETWEEN_YEARS_0_AND_1970);
        }
    }

private:
    ContextPtr ctx;
};


}

REGISTER_FUNCTION(FromDaysSinceYearZero)
{
    factory.registerFunction<FunctionFromDaysSinceYearZero<DateTraits>>(FunctionDocumentation{
        .description = R"(
Given the number of days passed since 1 January 0000 in the proleptic Gregorian calendar defined by ISO 8601 return a corresponding date.
The calculation is the same as in MySQL's FROM_DAYS() function. If an overflow of the range supported by Date were to happen the behaviour is controlled by DateTimeOverflowBehavior setting.
)",
        .examples{{"typical", "SELECT fromDaysSinceYearZero(713569)", "2023-09-08"}},
        .categories{"Dates and Times"}});

    factory.registerFunction<FunctionFromDaysSinceYearZero<DateTraits32>>(FunctionDocumentation{
        .description = R"(
Given the number of days passed since 1 January 0000 in the proleptic Gregorian calendar defined by ISO 8601 return a corresponding date.
The calculation is the same as in MySQL's FROM_DAYS() function. If an overflow of the range supported by Date32 were to happen the behaviour is controlled by DateTimeOverflowBehavior setting.
)",
        .examples{{"typical", "SELECT fromDaysSinceYearZero32(713569)", "2023-09-08"}},
        .categories{"Dates and Times"}});
}

}
