#include <Functions/FunctionHelpers.h>
#include <Functions/FunctionFactory.h>

#include <base/range.h>
#include <Interpreters/Context.h>
#include <Interpreters/ExternalModelsLoader.h>
#include <Columns/ColumnString.h>
#include <string>
#include <memory>
#include <DataTypes/DataTypeNullable.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnTuple.h>
#include <DataTypes/DataTypeTuple.h>
#include <Common/assert_cast.h>
#include <Functions/IFunction.h>
#include <Interpreters/Context_fwd.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int TOO_FEW_ARGUMENTS_FOR_FUNCTION;
    extern const int ILLEGAL_COLUMN;
}

class ExternalModelsLoader;


/// Evaluate external model.
/// First argument - model name, the others - model arguments.
///   * for CatBoost model - float features first, then categorical
/// Result - Float64.
class FunctionModelEvaluate final : public IFunction
{
public:
    static constexpr auto name = "modelEvaluate";

    static FunctionPtr create(ContextPtr context)
    {
        return std::make_shared<FunctionModelEvaluate>(context->getExternalModelsLoader());
    }

    explicit FunctionModelEvaluate(const ExternalModelsLoader & models_loader_)
        : models_loader(models_loader_) {}

    String getName() const override { return name; }

    bool isVariadic() const override { return true; }

    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }

    bool isDeterministic() const override { return false; }

    bool useDefaultImplementationForNulls() const override { return false; }

    size_t getNumberOfArguments() const override { return 0; }


    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if (arguments.size() < 2)
            throw Exception("Function " + getName() + " expects at least 2 arguments",
                            ErrorCodes::TOO_FEW_ARGUMENTS_FOR_FUNCTION);

        if (!isString(arguments[0].type))
            throw Exception("Illegal type " + arguments[0].type->getName() + " of first argument of function " + getName()
                            + ", expected a string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

        const auto * name_col = checkAndGetColumnConst<ColumnString>(arguments[0].column.get());
        if (!name_col)
            throw Exception("First argument of function " + getName() + " must be a constant string",
                            ErrorCodes::ILLEGAL_COLUMN);

        bool has_nullable = false;
        for (size_t i = 1; i < arguments.size(); ++i)
            has_nullable = has_nullable || arguments[i].type->isNullable();

        auto model = models_loader.getModel(name_col->getValue<String>());
        auto type = model->getReturnType();

        if (has_nullable)
        {
            if (const auto * tuple = typeid_cast<const DataTypeTuple *>(type.get()))
            {
                auto elements = tuple->getElements();
                for (auto & element : elements)
                    element = makeNullable(element);

                type = std::make_shared<DataTypeTuple>(elements);
            }
            else
                type = makeNullable(type);
        }

        return type;
    }


    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t) const override
    {
        const auto * name_col = checkAndGetColumnConst<ColumnString>(arguments[0].column.get());
        if (!name_col)
            throw Exception("First argument of function " + getName() + " must be a constant string",
                            ErrorCodes::ILLEGAL_COLUMN);

        auto model = models_loader.getModel(name_col->getValue<String>());

        ColumnRawPtrs column_ptrs;
        Columns materialized_columns;
        ColumnPtr null_map;

        column_ptrs.reserve(arguments.size());
        for (auto arg : collections::range(1, arguments.size()))
        {
            const auto & column = arguments[arg].column;
            column_ptrs.push_back(column.get());
            if (auto full_column = column->convertToFullColumnIfConst())
            {
                materialized_columns.push_back(full_column);
                column_ptrs.back() = full_column.get();
            }
            if (const auto * col_nullable = checkAndGetColumn<ColumnNullable>(*column_ptrs.back()))
            {
                if (!null_map)
                    null_map = col_nullable->getNullMapColumnPtr();
                else
                {
                    auto mut_null_map = IColumn::mutate(std::move(null_map));

                    NullMap & result_null_map = assert_cast<ColumnUInt8 &>(*mut_null_map).getData();
                    const NullMap & src_null_map = col_nullable->getNullMapColumn().getData();

                    for (size_t i = 0, size = result_null_map.size(); i < size; ++i)
                        if (src_null_map[i])
                            result_null_map[i] = 1;

                    null_map = std::move(mut_null_map);
                }

                column_ptrs.back() = &col_nullable->getNestedColumn();
            }
        }

        auto res = model->evaluate(column_ptrs);

        if (null_map)
        {
            if (const auto * tuple = typeid_cast<const ColumnTuple *>(res.get()))
            {
                auto nested = tuple->getColumns();
                for (auto & col : nested)
                    col = ColumnNullable::create(col, null_map);

                res = ColumnTuple::create(nested);
            }
            else
                res = ColumnNullable::create(res, null_map);
        }

        return res;
    }

private:
    const ExternalModelsLoader & models_loader;
};


REGISTER_FUNCTION(ExternalModels)
{
    factory.registerFunction<FunctionModelEvaluate>();
}

}
