#include "unicorn_test.h"

/* Standalone vendor check. Build with the same public headers and engine:
 * cc -Iinclude -I<build> tests/unit/test_arm_m8_monitor.c \
 *    -L<build> -Wl,-rpath,<build> -lunicorn -o <build>/test_arm_m8_monitor
 */
_Static_assert(UC_ARM_REG_R0 == 66, "keep existing register IDs");
_Static_assert(UC_ARM_REG_S31 == 110, "keep existing register IDs");
_Static_assert(UC_ARM_REG_CP_REG == 139, "keep existing register IDs");
_Static_assert(UC_ARM_REG_ESR == 140, "keep existing register IDs");
_Static_assert(UC_ARM_REG_EXCLUSIVE_ADDR == 141, "append vendor register IDs");
_Static_assert(UC_ARM_REG_EXCLUSIVE_HIGH == 143, "append vendor register IDs");

enum { CODE = 0x1000, DATA = 0x2000 };
static const char instructions[] =
    "\x51\xe8\x00\x0f" /* LDREX r0,[r1] */
    "\x00\xbf"         /* NOP */
    "\x41\xe8\x00\x32" /* STREX r2,r3,[r1] */
    "\xbf\xf3\x2f\x8f";/* CLREX */
static const int monitor_ids[] = {
    UC_ARM_REG_EXCLUSIVE_ADDR,
    UC_ARM_REG_EXCLUSIVE_VAL,
    UC_ARM_REG_EXCLUSIVE_HIGH
};
static const uint64_t high_word = UINT64_C(0xfedcba9876543210);

static uc_engine *setup(void)
{
    uc_engine *uc = NULL;
    uint32_t address = DATA, replacement = 0x76543210;
    uint32_t initial = LEINT32(0x12345678);
    OK(uc_open(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_M7));
    OK(uc_ctl_set_tcg_buffer_size(uc, 16 * 1024 * 1024));
    OK(uc_mem_map(uc, CODE, 0x2000, UC_PROT_ALL));
    OK(uc_mem_write(uc, CODE, instructions, sizeof(instructions) - 1));
    OK(uc_mem_write(uc, DATA, &initial, sizeof(initial)));
    OK(uc_reg_write(uc, UC_ARM_REG_R1, &address));
    OK(uc_reg_write(uc, UC_ARM_REG_R3, &replacement));
    OK(uc_reg_write(uc, UC_ARM_REG_EXCLUSIVE_HIGH, &high_word));
    return uc;
}

static uint64_t read_monitor(uc_engine *uc, int id)
{
    uint64_t value = 0;
    OK(uc_reg_read(uc, id, &value));
    return value;
}

static uint32_t read_word(uc_engine *uc)
{
    uint32_t value = 0;
    OK(uc_mem_read(uc, DATA, &value, sizeof(value)));
    return LEINT32(value);
}

static uint32_t read_status(uc_engine *uc)
{
    uint32_t value = 0;
    OK(uc_reg_read(uc, UC_ARM_REG_R2, &value));
    return value;
}

static void run(uc_engine *uc, uint32_t pc, uint32_t end)
{
    OK(uc_emu_start(uc, pc | 1u, end, 1000000, 1));
}

static void test_raw_width(void)
{
    uc_engine *uc = setup();
    const uint64_t values[] = {
        UINT64_MAX, UINT64_C(0x123456789abcdef0), high_word
    };
    unsigned i;
    TEST_CHECK(uc_m8_arm_exclusive_monitor_abi() == 1);
    for (i = 0; i < 3; ++i) {
        struct { uint64_t before, value, after; } guard = {
            UINT64_C(0xaaaaaaaaaaaaaaaa), 0, UINT64_C(0x5555555555555555)
        };
        size_t size = sizeof(uint64_t);
        OK(uc_reg_write2(uc, monitor_ids[i], &values[i], &size));
        TEST_CHECK(size == sizeof(uint64_t));
        size = sizeof(uint64_t);
        OK(uc_reg_read2(uc, monitor_ids[i], &guard.value, &size));
        TEST_CHECK(size == sizeof(uint64_t));
        TEST_CHECK(guard.value == values[i]);
        TEST_CHECK(guard.before == UINT64_C(0xaaaaaaaaaaaaaaaa));
        TEST_CHECK(guard.after == UINT64_C(0x5555555555555555));
        guard.value = 7;
        size = sizeof(uint32_t);
        uc_assert_err(UC_ERR_OVERFLOW,
            uc_reg_read2(uc, monitor_ids[i], &guard.value, &size));
        TEST_CHECK(guard.value == 7);
        size = sizeof(uint32_t);
        uc_assert_err(UC_ERR_OVERFLOW,
            uc_reg_write2(uc, monitor_ids[i], &guard.value, &size));
        TEST_CHECK(read_monitor(uc, monitor_ids[i]) == values[i]);
    }
    OK(uc_close(uc));
}

static void test_real_pair_and_ordinary_entry(void)
{
    uc_engine *uc = setup();
    uint32_t r4 = 0xabcdef01, pc = (CODE + 4u) | 1u;
    run(uc, CODE, CODE + 4);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_ADDR) == DATA);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_VAL) == 0x12345678);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_HIGH) == high_word);
    OK(uc_reg_write(uc, UC_ARM_REG_R4, &r4));
    OK(uc_reg_write(uc, UC_ARM_REG_PC, &pc));
    run(uc, CODE + 4, CODE + 6);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_ADDR) == DATA);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_VAL) == 0x12345678);
    run(uc, CODE + 6, CODE + 10);
    TEST_CHECK(read_status(uc) == 0);
    TEST_CHECK(read_word(uc) == 0x76543210);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_ADDR) == UINT64_MAX);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_VAL) == 0x12345678);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_HIGH) == high_word);
    OK(uc_close(uc));
}

static void test_context_real_reservation(void)
{
    uc_engine *uc = setup();
    uc_context *context = NULL;
    run(uc, CODE, CODE + 4);
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));
    run(uc, CODE + 10, CODE + 14);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_ADDR) == UINT64_MAX);
    OK(uc_context_restore(uc, context));
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_ADDR) == DATA);
    run(uc, CODE + 6, CODE + 10);
    TEST_CHECK(read_status(uc) == 0);
    TEST_CHECK(read_word(uc) == 0x76543210);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_HIGH) == high_word);
    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void test_strex_failures(void)
{
    unsigned mismatch;
    for (mismatch = 0; mismatch < 3; ++mismatch) {
        uc_engine *uc = setup();
        uint64_t address = mismatch == 0 ? UINT64_MAX : DATA;
        uint64_t value = 0x12345678;
        if (mismatch == 1) address += 4;
        if (mismatch == 2) value += 1;
        OK(uc_reg_write(uc, UC_ARM_REG_EXCLUSIVE_ADDR, &address));
        OK(uc_reg_write(uc, UC_ARM_REG_EXCLUSIVE_VAL, &value));
        run(uc, CODE + 6, CODE + 10);
        TEST_CHECK(read_status(uc) == 1);
        TEST_CHECK(read_word(uc) == 0x12345678);
        TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_ADDR) == UINT64_MAX);
        TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_VAL) == value);
        TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_HIGH) == high_word);
        OK(uc_close(uc));
    }
}

static void test_clrex(void)
{
    uc_engine *uc = setup();
    uint64_t value;
    run(uc, CODE, CODE + 4);
    value = read_monitor(uc, UC_ARM_REG_EXCLUSIVE_VAL);
    run(uc, CODE + 10, CODE + 14);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_ADDR) == UINT64_MAX);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_VAL) == value);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_HIGH) == high_word);
    run(uc, CODE + 6, CODE + 10);
    TEST_CHECK(read_status(uc) == 1);
    TEST_CHECK(read_word(uc) == 0x12345678);
    OK(uc_close(uc));
}

static void test_context(void)
{
    uc_engine *uc = setup();
    uc_context *context = NULL;
    const uint64_t values[] = {
        UINT64_C(0x0123456789abcdef), UINT64_C(0x9876543210abcdef), high_word
    };
    unsigned i;
    OK(uc_context_alloc(uc, &context));
    for (i = 0; i < 3; ++i)
        OK(uc_reg_write(uc, monitor_ids[i], &values[i]));
    OK(uc_context_save(uc, context));
    for (i = 0; i < 3; ++i) {
        uint64_t saved = 0, changed = values[i] ^ UINT64_MAX;
        size_t size = sizeof(uint64_t);
        OK(uc_context_reg_read2(context, monitor_ids[i], &saved, &size));
        TEST_CHECK(saved == values[i]);
        TEST_CHECK(size == sizeof(uint64_t));
        OK(uc_reg_write(uc, monitor_ids[i], &changed));
    }
    OK(uc_context_restore(uc, context));
    for (i = 0; i < 3; ++i) {
        uint64_t changed = values[i] ^ UINT64_MAX;
        size_t size = sizeof(uint64_t);
        TEST_CHECK(read_monitor(uc, monitor_ids[i]) == values[i]);
        OK(uc_context_reg_write2(context, monitor_ids[i], &changed, &size));
    }
    OK(uc_context_restore(uc, context));
    for (i = 0; i < 3; ++i)
        TEST_CHECK(read_monitor(uc, monitor_ids[i]) == (values[i] ^ UINT64_MAX));
    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void test_imported_reservation(void)
{
    uc_engine *uc = setup();
    uint64_t address = DATA, value = 0x12345678;
    /* A native LDREX can publish a reservation before Unicorn STREX. */
    OK(uc_reg_write(uc, UC_ARM_REG_EXCLUSIVE_ADDR, &address));
    OK(uc_reg_write(uc, UC_ARM_REG_EXCLUSIVE_VAL, &value));
    run(uc, CODE + 6, CODE + 10);
    TEST_CHECK(read_status(uc) == 0);
    TEST_CHECK(read_word(uc) == 0x76543210);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_ADDR) == UINT64_MAX);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_HIGH) == high_word);
    OK(uc_close(uc));
}

static void test_export_completed_pair(void)
{
    uc_engine *uc = setup();
    uint64_t invalid = UINT64_MAX;
    /* An inherited reservation must not survive a native completed pair. */
    run(uc, CODE, CODE + 4);
    OK(uc_reg_write(uc, UC_ARM_REG_EXCLUSIVE_ADDR, &invalid));
    run(uc, CODE + 6, CODE + 10);
    TEST_CHECK(read_status(uc) == 1);
    TEST_CHECK(read_word(uc) == 0x12345678);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_VAL) == 0x12345678);
    TEST_CHECK(read_monitor(uc, UC_ARM_REG_EXCLUSIVE_HIGH) == high_word);
    OK(uc_close(uc));
}

TEST_LIST = {
    {"raw_64bit_width", test_raw_width},
    {"real_pair_and_ordinary_entry", test_real_pair_and_ordinary_entry},
    {"strex_failures", test_strex_failures},
    {"clrex", test_clrex},
    {"context", test_context},
    {"context_real_reservation", test_context_real_reservation},
    {"imported_reservation", test_imported_reservation},
    {"export_completed_pair", test_export_completed_pair},
    {NULL, NULL}
};
