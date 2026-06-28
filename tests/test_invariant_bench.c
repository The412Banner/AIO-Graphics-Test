#include <check.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

// Include the actual production header
#include "src/bench.h"

START_TEST(test_allocation_size_overflow_check)
{
    // Invariant: Multiplication for allocation size must not overflow
    // or must be properly validated before allocation
    
    // Test cases: boundary values that could cause overflow
    size_t test_cases[][2] = {
        // Valid case
        {100, 10},
        // Boundary case near SIZE_MAX
        {SIZE_MAX / 100, 101},
        // Exact exploit case - multiplication wraps around
        {SIZE_MAX / 2 + 1, 2},
        // Another boundary case
        {SIZE_MAX, 1},
        // Zero case (should be handled)
        {0, SIZE_MAX}
    };
    
    int num_cases = sizeof(test_cases) / sizeof(test_cases[0]);
    
    for (int i = 0; i < num_cases; i++) {
        size_t a = test_cases[i][0];
        size_t b = test_cases[i][1];
        
        // The security property: allocation size calculation must be safe
        // We're testing that the function either:
        // 1. Properly checks for overflow before allocating
        // 2. Uses safe multiplication that doesn't overflow
        // 3. Handles the allocation failure gracefully
        
        // Call the actual function from bench.c
        // This assumes there's a function that performs the allocation
        // based on the multiplication of two size_t values
        void *result = allocate_with_size_check(a, b);
        
        // If allocation succeeded, the size must have been calculated safely
        if (result != NULL) {
            // Verify the allocation size is reasonable
            // This is a sanity check - the real test is that the function
            // doesn't crash or overflow due to unsafe multiplication
            ck_assert_msg(a == 0 || b == 0 || a <= SIZE_MAX / b,
                         "Unsafe multiplication detected: %zu * %zu could overflow",
                         a, b);
            free(result);
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_allocation_size_overflow_check);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}