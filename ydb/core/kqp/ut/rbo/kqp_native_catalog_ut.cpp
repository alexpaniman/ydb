#include <ydb/core/kqp/opt/rbo/kqp_native_catalog.h>
#include <ydb/core/kqp/opt/rbo/kqp_rbo_table.h>
#include <ydb/core/kqp/provider/yql_kikimr_gateway.h>
#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NKqp {
using namespace NFrontend;

Y_UNIT_TEST_SUITE(KqpNativeCatalog) {
    Y_UNIT_TEST(StorageTypesMatchThePreviousTextImport) {
        for (const auto id : NScheme::NTypeIds::YqlIds) {
            if (id == NScheme::NTypeIds::DyNumber || id == NScheme::NTypeIds::Decimal) continue;
            for (const bool notNull : {false, true}) {
                auto expected = ParseType(NScheme::TypeName(id));
                expected.OptionalDepth = !notNull;
                UNIT_ASSERT(ImportNativeStorageType(NScheme::TTypeInfo(id), notNull) == expected);
            }
        }
        for (unsigned precision = 1; precision <= 35; ++precision) for (unsigned scale = 0; scale <= precision; ++scale)
            for (const bool notNull : {false, true}) {
                const auto expected = ParseType("Decimal(" + std::to_string(precision) + "," + std::to_string(scale) + ")" + (notNull ? "" : "?"));
                UNIT_ASSERT(ImportNativeStorageType(NScheme::TDecimalType(precision, scale), notNull) == expected);
            }
    }

    Y_UNIT_TEST(InvalidAndUnsupportedStorageTypesRetainTheSourcePosition) {
        const TSourcePosition position{31, 4, 8};
        for (const auto type : {NScheme::TTypeInfo(NScheme::NTypeIds::DyNumber),
                NScheme::TTypeInfo(static_cast<const NPg::ITypeDesc*>(nullptr)), NScheme::TTypeInfo(0xffff)}) {
            try { ImportNativeStorageType(type, true, position); UNIT_FAIL("unsupported storage type accepted"); }
            catch (const TUnsupportedFeature& error) {
                UNIT_ASSERT(error.Feature == EFeature::Type);
                UNIT_ASSERT_VALUES_EQUAL(error.Position.Offset, position.Offset);
                UNIT_ASSERT_VALUES_EQUAL(error.Position.Line, position.Line);
                UNIT_ASSERT_VALUES_EQUAL(error.Position.Column, position.Column);
            }
        }
        try { ImportNativeStorageType({}, false, position); UNIT_FAIL("missing storage type accepted"); }
        catch (const TUnsupportedFeature&) { UNIT_FAIL("missing catalog metadata must not select fallback"); }
        catch (const TFrontendError& error) { UNIT_ASSERT_VALUES_EQUAL(error.Position.Offset, position.Offset); }
    }

    Y_UNIT_TEST(IndexCatalogUsesTypedMetadataAndOwnsTheImportedSchema) {
        const auto table = [] {
            NYql::TKikimrTableMetadata metadata;
            metadata.Name = "/Root/index";
            auto& id = metadata.Columns["id"];
            id.TypeInfo = NScheme::TTypeInfo(NScheme::NTypeIds::Uint64); id.NotNull = true;
            auto& amount = metadata.Columns["amount"];
            amount.TypeInfo = NScheme::TDecimalType(12, 3);
            // Printable Type is deliberately absent: storage metadata is the
            // authoritative catalog contract, including index implementation tables.
            return MakeRboTable(metadata);
        }();
        UNIT_ASSERT(table.GetColumnType("id") == TType{ETypeKind::Uint64});
        UNIT_ASSERT(table.GetColumnType("amount") == (TType{ETypeKind::Decimal, 1, 12, 3}));
    }
}
} // namespace NKikimr::NKqp
