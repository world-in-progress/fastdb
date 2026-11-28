package io.fastdb.payload;

/** Logical input values for the shared Core binary fixtures. */
final class GoldenScenarios {
    private GoldenScenarios() {}

    private static final byte[] EMPTY = {};
    private static final float F32_NAN = Float.intBitsToFloat(0x7fa12345);
    private static final double F64_NAN = Double.longBitsToDouble(0x7ff0000000000042L);

    static void author(CompiledSpec spec, Builder builder, String name) {
        switch (name) {
            case "empty" -> { }
            case "fixed_scalars" -> fixedScalars(builder);
            case "numeric_edges" -> numericEdges(builder);
            case "nested_components" -> nestedComponents(builder);
            case "text_bytes" -> textBytes(builder);
            case "nested_lists" -> nestedLists(builder);
            case "component_list_composition" -> componentLists(builder);
            case "component_list_empty" -> builder.entryBegin(0, 0).entryBegin(1, 0);
            case "graph_empty" -> builder.entryBegin(0, 0);
            case "graph_all_values" -> graphAllValues(builder, spec.componentIndex("Node"),
                    spec.componentIndex("Asset"));
            case "graph_nested_lists" -> graphNestedLists(builder, spec.componentIndex("Node"));
            case "graph_disconnected_roots" -> graphDisconnected(builder, spec.componentIndex("Node"));
            case "graph_null_empty" -> graphNullEmpty(builder, spec.componentIndex("Node"));
            case "graph_single_root" -> graphSingleRoot(builder, spec.componentIndex("Node"));
            case "graph_shared_cycle" -> graphSharedCycle(builder, spec.componentIndex("Node"));
            default -> throw new AssertionError("Unknown binary golden scenario " + name);
        }
    }

    private static void fixedScalars(Builder b) {
        b.entryBegin(0, 1).valueBool(true)
         .entryBegin(1, 1).valueU8(0xab)
         .entryBegin(2, 3).valueU16(0x1234).valueNull().valueU16(0xffff)
         .entryBegin(3, 1).valueU32(0x12345678)
         .entryBegin(4, 1).valueI32(-2)
         .entryBegin(5, 4).valueF32(-0.0f).valueF32(Float.POSITIVE_INFINITY)
         .valueF32(Float.NEGATIVE_INFINITY).valueF32(F32_NAN)
         .entryBegin(6, 4).valueF64(-0.0).valueF64(Double.POSITIVE_INFINITY)
         .valueF64(Double.NEGATIVE_INFINITY).valueF64(F64_NAN);
    }

    private static void numericEdges(Builder b) {
        b.entryBegin(0, 2).valueBool(false).valueBool(true)
         .entryBegin(1, 2).valueU8(0).valueU8(255)
         .entryBegin(2, 2).valueU16(0).valueU16(65535)
         .entryBegin(3, 2).valueU32(0).valueU32(0xffff_ffffL)
         .entryBegin(4, 2).valueI32(Integer.MIN_VALUE).valueI32(Integer.MAX_VALUE)
         .entryBegin(5, 6).valueF32(0).valueF32(-0.0f).valueF32(Float.POSITIVE_INFINITY)
         .valueF32(Float.NEGATIVE_INFINITY).valueF32(F32_NAN).valueF32(Float.NaN)
         .entryBegin(6, 6).valueF64(0).valueF64(-0.0).valueF64(Double.POSITIVE_INFINITY)
         .valueF64(Double.NEGATIVE_INFINITY).valueF64(F64_NAN).valueF64(Double.NaN)
         .entryBegin(7, 6).valueU8n(0).valueU8n(-0.0).valueU8n(255).valueU8n(0.5)
         .valueU8n(1.5).valueU8n(Double.MIN_VALUE)
         .entryBegin(8, 4).valueU16n(-32768).valueU16n(32767).valueU16n(-32767.5).valueU16n(-32766.5);
    }

    private static void startOuter(Builder b) {
        b.valueComponentBegin().valueComponentBegin().valueComponentBegin();
    }

    private static void leaf(Builder b, Boolean flag, double code, double value) {
        b.valueComponentBegin();
        if (flag == null) {
            b.valueNull();
        } else {
            b.valueBool(flag);
        }
        b.valueU16n(code).valueF64(value);
    }

    private static void nestedComponents(Builder b) {
        b.entryBegin(0, 1);
        startOuter(b);
        b.valueNull();
        leaf(b, true, 65535, F64_NAN);
        leaf(b, null, 0.5, -0.0);
        b.valueU32(0xffff_ffffL);

        b.entryBegin(1, 3).valueNull();
        startOuter(b);
        leaf(b, false, 0, Double.POSITIVE_INFINITY);
        leaf(b, true, 1.5, Double.NEGATIVE_INFINITY);
        b.valueNull().valueU32(0);

        startOuter(b);
        b.valueNull();
        leaf(b, null, 65535, 0);
        leaf(b, false, 1.5, Double.NaN);
        b.valueU32(42);

        b.entryBegin(2, 2).valueComponentBegin().valueComponentBegin();
    }

    private static void textBytes(Builder b) {
        byte[] opaque = {0, (byte) 0xff, (byte) 0x80};
        byte[] nested = {0, (byte) 0xff, (byte) 0x80, 0x41};
        b.entryBegin(0, 7).valueNull().valueStr("").valueStr("ASCII").valueStr("中\0")
         .valueStr("repeat").valueStr("repeat").valueStr("tail")
         .entryBegin(1, 6).valueNull().valueWstr("").valueWstr("A").valueWstr("中\0")
         .valueWstr("😃").valueWstr("A")
         .entryBegin(2, 6).valueNull().valueBytes(EMPTY).valueBytes(opaque).valueBytes(opaque)
         .valueBytes(new byte[] {0x41, 0}).valueBytes(EMPTY)
         .entryBegin(3, 2).valueComponentBegin().valueStr("P").valueComponentBegin().valueStr("I")
         .valueWstr("A中😃\0").valueBytes(nested).valueBytes(new byte[] {1, 2})
         .valueComponentBegin().valueStr("").valueComponentBegin().valueStr("repeat")
         .valueWstr("A").valueBytes(nested).valueBytes(EMPTY);
    }

    private static void nestedLists(Builder b) {
        b.entryBegin(0, 2).valueComponentBegin()
         .valueListBegin(3).valueNull().valueListBegin(0)
         .valueListBegin(3).valueStr("").valueNull().valueStr("alpha")
         .valueNull().valueListBegin(0)
         .valueComponentBegin().valueListBegin(0)
         .valueListBegin(3).valueNull().valueWstr("").valueWstr("A😃")
         .valueListBegin(2).valueNull().valueBytes(new byte[] {0, (byte) 0xff, 0x41})
         .entryBegin(1, 3).valueNull().valueListBegin(0)
         .valueListBegin(3).valueU16(7).valueNull().valueU16(9)
         .entryBegin(2, 1).valueListBegin(3).valueNull().valueListBegin(0)
         .valueListBegin(3).valueStr("").valueNull().valueStr("tail");
    }

    private static void componentLists(Builder b) {
        byte[] opaque = {0, (byte) 0xff, (byte) 0x80};
        b.entryBegin(0, 2).valueComponentBegin().valueComponentBegin()
         .valueBool(true).valueU8(8).valueU16(16).valueU32(32).valueI32(-32)
         .valueU8n(0.5).valueU16n(0).valueF32(1).valueF64(2)
         .valueStr("component").valueWstr("中").valueBytes(opaque)
         .valueListBegin(3).valueBool(true).valueNull().valueBool(false)
         .valueListBegin(2).valueU8(0).valueU8(255)
         .valueListBegin(2).valueU16(0).valueU16(65535)
         .valueListBegin(2).valueU32(0).valueU32(0xffff_ffffL)
         .valueListBegin(2).valueI32(Integer.MIN_VALUE).valueI32(Integer.MAX_VALUE)
         .valueListBegin(2).valueU8n(0).valueU8n(1)
         .valueListBegin(2).valueU16n(-1).valueU16n(1)
         .valueListBegin(2).valueF32(-0.0f).valueF32(F32_NAN)
         .valueListBegin(2).valueF64(Double.POSITIVE_INFINITY).valueF64(Double.NaN)
         .valueListBegin(3).valueStr("").valueNull().valueStr("list")
         .valueListBegin(2).valueNull().valueWstr("中")
         .valueListBegin(2).valueNull().valueBytes(opaque)
         .valueListBegin(3).valueNull().valueComponentBegin().valueU32(1).valueNull()
         .valueComponentBegin().valueU32(2).valueStr("two")
         .valueListBegin(3).valueNull().valueListBegin(0)
         .valueListBegin(3).valueU8(1).valueNull().valueU8(2)
         .valueComponentBegin().valueU32(9).valueStr("root");

        b.valueComponentBegin().valueComponentBegin()
         .valueBool(false).valueU8(0).valueU16(0).valueU32(0).valueI32(0)
         .valueU8n(0).valueU16n(0).valueF32(0).valueF64(0)
         .valueStr("").valueWstr("").valueBytes(EMPTY);
        for (int list = 0; list < 12; list++) {
            b.valueListBegin(0);
        }
        b.valueNull().valueListBegin(0).valueNull()
         .entryBegin(1, 3).valueNull().valueListBegin(0)
         .valueListBegin(2).valueNull().valueComponentBegin().valueU32(7).valueStr("entry");
    }

    private static void graphAllValues(Builder b, int node, int asset) {
        graphAllValues(b, b.declareObject(node), b.declareObject(asset));
    }

    static void graphAllValues(Builder b, ObjectHandle root, ObjectHandle child) {
        b.objectFillBegin(root).valueBool(true).valueU8(0x12).valueU16(0x3456)
         .valueU32(0x789abcde).valueI32(-1234567).valueU8n(0.5).valueU16n(0)
         .valueF32(F32_NAN).valueF64(Double.NaN)
         .valueStr("same").valueWstr("A😀").valueBytes(new byte[] {0, (byte) 0xff, 0x7e})
         .valueComponentBegin().valueNull().valueU16(0xbeef)
         .valueListBegin(3).valueF32(-0.0f).valueNull().valueF32(Float.NaN)
         .valueRef(root).valueRef(child);
        b.objectFillBegin(child).valueStr("same").valueRef(root)
         .entryBegin(0, 1).valueObject(root).entryBegin(1, 1).valueObject(child)
         .entryBegin(2, 2).valueRef(root).valueNull()
         .entryBegin(3, 3).valueU8n(0).valueU8n(0.5).valueU8n(1)
         .entryBegin(4, 3).valueU16n(-1).valueU16n(0).valueU16n(1);
    }

    private static void graphNestedLists(Builder b, int node) {
        ObjectHandle root = b.declareObject(node);
        b.objectFillBegin(root)
         .valueListBegin(3).valueListBegin(2).valueU16(1).valueNull().valueNull().valueListBegin(0)
         .valueComponentBegin().valueListBegin(2).valueListBegin(2).valueU8(7).valueNull().valueListBegin(0)
         .entryBegin(0, 1).valueObject(root)
         .entryBegin(1, 3).valueListBegin(2).valueListBegin(1).valueU16(9).valueNull()
         .valueNull().valueListBegin(0);
    }

    private static void graphDisconnected(Builder b, int node) {
        ObjectHandle first = b.declareObject(node);
        ObjectHandle second = b.declareObject(node);
        b.objectFillBegin(first).valueU32(11).valueNull()
         .objectFillBegin(second).valueU32(22).valueNull()
         .entryBegin(0, 2).valueObject(first).valueObject(second);
    }

    private static void graphNullEmpty(Builder b, int node) {
        ObjectHandle root = b.declareObject(node);
        b.objectFillBegin(root).valueNull().valueWstr("").valueBytes(EMPTY)
         .valueListBegin(0).valueNull().valueNull()
         .entryBegin(0, 2).valueObject(root).valueNull();
    }

    private static void graphSingleRoot(Builder b, int node) {
        ObjectHandle root = b.declareObject(node);
        b.objectFillBegin(root).valueU16(0x1234).valueNull()
         .entryBegin(0, 1).valueObject(root).entryBegin(1, 1).valueNull();
    }

    private static void graphSharedCycle(Builder b, int node) {
        ObjectHandle first = b.declareObject(node);
        ObjectHandle second = b.declareObject(node);
        b.objectFillBegin(first).valueU32(100).valueRef(first).valueRef(second)
         .valueComponentBegin().valueBool(true).valueU16(0x1234)
         .objectFillBegin(second).valueU32(200).valueRef(second).valueRef(second)
         .valueComponentBegin().valueBool(false).valueU16(0xabcd)
         .entryBegin(0, 1).valueObject(first).entryBegin(1, 1).valueObject(second);
    }
}
