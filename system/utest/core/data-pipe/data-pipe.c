// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <magenta/syscalls.h>
#include <unittest/unittest.h>

static uint32_t lcg_rand(uint32_t seed) {
    return (seed = (seed * 1664525u) + 1013904223u);
}

// Fill a region of memory with a pattern. The seed is returned
// so that the fill can be done in chunks. When done so, you need to
// store the seed if you want to test the memory in chunks.
static uint32_t fill_region(void* _ptr, size_t len, uint32_t seed) {
    uint32_t* ptr = (uint32_t*)_ptr;
    uint32_t val = seed;

    for (size_t i = 0; i < len / 4; i++) {
        ptr[i] = val;
        val = lcg_rand(val);
    }
    return val;
}

// Test a region of memory against a a fill with fill_region().
static bool test_region(void* _ptr, size_t len, uint32_t seed) {
    uint32_t* ptr = (uint32_t*)_ptr;
    uint32_t val = seed;

    for (size_t i = 0; i < len / 4; i++) {
        if (ptr[i] != val) {
            unittest_printf("wrong value at %p (%zu): 0x%x vs 0x%x\n", &ptr[i], i, ptr[i], val);
            return false;
        }
        val = lcg_rand(val);
    }
    return true;
}

#define KB_(x) (x*1024)

static mx_signals_t get_satisfied_signals(mx_handle_t handle) {
    mx_signals_state_t signals_state = {0};
    mx_handle_wait_one(handle, 0u, 0u, &signals_state);
    return signals_state.satisfied;
}

static mx_signals_t get_satisfiable_signals(mx_handle_t handle) {
    mx_signals_state_t signals_state = {0};
    mx_handle_wait_one(handle, 0u, 0u, &signals_state);
    return signals_state.satisfiable;
}

static bool create_destroy_test(void) {
    BEGIN_TEST;
    mx_status_t status;
    mx_handle_t producer;
    mx_handle_t consumer;

    producer = mx_datapipe_create(0u, 1u, KB_(1), &consumer);
    ASSERT_GT(producer, 0, "could not create producer data pipe");
    ASSERT_GT(consumer, 0, "could not create consumer data pipe");

    ASSERT_EQ(get_satisfied_signals(consumer), 0u, "");
    ASSERT_EQ(get_satisfied_signals(producer), MX_SIGNAL_WRITABLE, "");

    ASSERT_EQ(get_satisfiable_signals(consumer), MX_SIGNAL_READABLE | MX_SIGNAL_PEER_CLOSED, "");
    ASSERT_EQ(get_satisfiable_signals(producer), MX_SIGNAL_WRITABLE | MX_SIGNAL_PEER_CLOSED, "");

    status = mx_datapipe_end_write(producer, 0u);
    ASSERT_EQ(status, ERR_BAD_STATE, "wrong pipe state");
    status = mx_datapipe_end_read(consumer, 0u);
    ASSERT_EQ(status, ERR_BAD_STATE, "wrong pipe state");

    uintptr_t buffer = 0;
    mx_ssize_t avail;

    // TODO(cpu): re-enable this code when we have fine grained
    // control over MX_PROP_BAD_HANDLE_POLICY in the launcher.
#if 0
    avail = mx_datapipe_begin_write(consumer, 0u, &buffer);
    ASSERT_EQ(avail, ERR_BAD_HANDLE, "expected error");
    avail = mx_datapipe_begin_read(producer, 0u, &buffer);
    ASSERT_EQ(avail, ERR_BAD_HANDLE, "expected error");
#endif

    avail = mx_datapipe_write(producer, 0u, 10u, "0123456789");
    ASSERT_EQ(avail, 10, "expected success");

    avail = mx_datapipe_begin_write(producer, 0u, &buffer);
    ASSERT_EQ(avail, KB_(1) - 10, "expected success");

    status = mx_handle_close(producer);
    ASSERT_GE(status, NO_ERROR, "failed to close data pipe");
    status = mx_handle_close(consumer);
    ASSERT_GE(status, NO_ERROR, "failed to close data pipe");
    END_TEST;
}

static bool loop_write_full(void) {
    BEGIN_TEST;
    mx_handle_t producer;
    mx_handle_t consumer;
    mx_status_t status;

    producer = mx_datapipe_create(0u, 1u, KB_(32), &consumer);
    ASSERT_GT(producer, 0, "could not create producer data pipe");
    ASSERT_GT(consumer, 0, "could not create consumer data pipe");

    for (int ix = 0; ; ++ix) {
        uintptr_t buffer = 0;
        mx_ssize_t avail = mx_datapipe_begin_write(producer, 0u, &buffer);
        if (avail < 0) {
            ASSERT_EQ(avail, ERR_SHOULD_WAIT, "wrong error");
            ASSERT_EQ(ix, 8, "wrong capacity");
            break;
        }
        memset((void*)buffer, ix, KB_(4));
        status = mx_datapipe_end_write(producer, KB_(4));
        ASSERT_EQ(status, NO_ERROR, "failed to end write");
    }

    ASSERT_EQ(get_satisfied_signals(producer), 0u, "");
    ASSERT_EQ(get_satisfiable_signals(producer), MX_SIGNAL_WRITABLE | MX_SIGNAL_PEER_CLOSED, "");

    status = mx_handle_close(consumer);
    ASSERT_GE(status, NO_ERROR, "failed to close data pipe");

    ASSERT_EQ(get_satisfied_signals(producer), MX_SIGNAL_PEER_CLOSED, "");
    ASSERT_EQ(get_satisfiable_signals(producer), MX_SIGNAL_PEER_CLOSED, "");

    status = mx_handle_close(producer);
    ASSERT_GE(status, NO_ERROR, "failed to close data pipe");
    END_TEST;
}

static bool simple_read_write(void) {
    BEGIN_TEST;

    mx_handle_t producer;
    mx_handle_t consumer;
    mx_status_t status;

    producer = mx_datapipe_create(0u, 1u, KB_(4), &consumer);
    ASSERT_GT(producer, 0, "data pipe creation failed");
    ASSERT_GT(consumer, 0, "data pipe creation failed");

    mx_ssize_t written = mx_datapipe_write(producer, 0u, 4, "hello");
    ASSERT_EQ(written, 4, "write failed");

    status = mx_handle_close(producer);
    ASSERT_EQ(status, NO_ERROR, "");

    char buffer[64];
    mx_ssize_t read = mx_datapipe_read(consumer, 0u, 1, buffer);
    ASSERT_EQ(read, 1, "read failed");

    uintptr_t bb;
    read = mx_datapipe_begin_read(consumer, 0u, &bb);
    ASSERT_EQ(read, 3, "begin read failed");

    memcpy(&buffer[1], (char*)bb, 3u);
    int eq = memcmp(buffer, "hell", 4u);
    ASSERT_EQ(eq, 0, "");

    status = mx_datapipe_end_read(consumer, 3u);
    ASSERT_EQ(status, NO_ERROR, "end read failed");

    status = mx_handle_close(consumer);
    ASSERT_EQ(status, NO_ERROR, "close failed");

    END_TEST;
}

static bool write_read(void) {
    // Pipe of 32KB. Single write of 12000 bytes and 4 reads of 3000 bytes each.
    BEGIN_TEST;
    mx_handle_t producer;
    mx_handle_t consumer;
    mx_status_t status;

    producer = mx_datapipe_create(0u, 1u, KB_(32), &consumer);
    ASSERT_GT(producer, 0, "could not create producer data pipe");
    ASSERT_GT(consumer, 0, "could not create consumer data pipe");

    char* buffer = (char*) malloc(4 * 3000u);
    ASSERT_NEQ(buffer, NULL, "failed to alloc");

    uint32_t seed[5] = {7u, 0u, 0u, 0u, 0u};
    char* f = buffer;
    for (int ix = 0; ix != 4; ++ix) {
        seed[ix + 1] = fill_region(f, 3000u, seed[ix]);
        f += 3000u;
    }

    mx_ssize_t written = mx_datapipe_write(producer, 0u, 4 * 3000u, buffer);
    ASSERT_EQ(written, 4 * 3000, "write failed");

    ASSERT_EQ(get_satisfied_signals(consumer), MX_SIGNAL_READABLE, "");

    status = mx_handle_close(producer);
    ASSERT_GE(status, NO_ERROR, "failed to close data pipe");

    ASSERT_EQ(get_satisfied_signals(consumer), MX_SIGNAL_READABLE | MX_SIGNAL_PEER_CLOSED, "");

    memset(buffer, 0, 4 * 3000u);

    for (int ix= 0; ix != 4; ++ix) {
        mx_ssize_t read = mx_datapipe_read(consumer, 0u, 3000u, buffer);
        ASSERT_EQ(read, 3000, "read failed");

        bool equal = test_region((void*)buffer, 3000u, seed[ix]);
        ASSERT_EQ(equal, true, "invalid data");
    }

    free(buffer);

    status = mx_handle_close(consumer);
    ASSERT_GE(status, NO_ERROR, "failed to close data pipe");
    END_TEST;
}

static bool begin_write_read(void) {
    // Pipe of 32KB. Single write of 12000 bytes and 4 reads of 3000 bytes each.
    BEGIN_TEST;
    mx_handle_t producer;
    mx_handle_t consumer;
    mx_status_t status;

    producer = mx_datapipe_create(0u, 1u, KB_(32), &consumer);
    ASSERT_GE(producer, 0, "could not create producer data pipe");
    ASSERT_GE(consumer, 0, "could not create consumer data pipe");

    uintptr_t buffer = 0;
    mx_ssize_t avail = mx_datapipe_begin_write(producer, 0u, &buffer);
    ASSERT_EQ(avail, KB_(32), "begin_write failed");

    uint32_t seed[5] = {7u, 0u, 0u, 0u, 0u};
    for (int ix = 0; ix != 4; ++ix) {
        seed[ix + 1] = fill_region((void*)buffer, 3000u, seed[ix]);
        buffer += 3000u;
    }

    status = mx_datapipe_end_write(producer, 12000u);
    ASSERT_EQ(status, NO_ERROR, "failed to end write");

    status = mx_handle_close(producer);
    ASSERT_GE(status, NO_ERROR, "failed to close data pipe");

    for (int ix = 0; ix != 4; ++ix) {
        buffer = 0;
        avail = mx_datapipe_begin_read(consumer, 0u, &buffer);
        ASSERT_EQ(avail, 12000 - ix * 3000, "begin_read failed");

        bool equal = test_region((void*)buffer, 3000u, seed[ix]);
        ASSERT_EQ(equal, true, "invalid data");

        status = mx_datapipe_end_read(consumer, 3000u);
        ASSERT_EQ(status, NO_ERROR, "failed to end read");
    }

    status = mx_handle_close(consumer);
    ASSERT_GE(status, NO_ERROR, "failed to close data pipe");
    END_TEST;
}

static bool loop_write_read(void) {
    BEGIN_TEST;
    mx_handle_t producer;
    mx_handle_t consumer;
    mx_status_t status;

    producer = mx_datapipe_create(0u, 1u, KB_(36), &consumer);
    ASSERT_GT(producer, 0, "could not create producer data pipe");
    ASSERT_GT(consumer, 0, "could not create consumer data pipe");

    char* buffer = (char*) malloc(KB_(16));

    // The writer goes faster, after 10 rounds the write cursor catches up from behind.
    for (int ix = 0; ; ++ix) {
        mx_ssize_t written = mx_datapipe_write(producer, 0u, KB_(12), buffer);
        if (written != KB_(12)) {
            ASSERT_EQ(ix, 9, "bad cursor management");
            ASSERT_EQ(written, KB_(9), "bad capacity");
            break;
        }

        mx_ssize_t read = mx_datapipe_read(consumer, 0u, KB_(9), buffer);
        ASSERT_EQ(read, KB_(9), "read failed");
    }

    status = mx_handle_close(consumer);
    ASSERT_GE(status, NO_ERROR, "failed to close data pipe");
    status = mx_handle_close(producer);
    ASSERT_GE(status, NO_ERROR, "failed to close data pipe");
    END_TEST;
}

static bool loop_begin_write_read(void) {
    BEGIN_TEST;
    mx_handle_t producer;
    mx_handle_t consumer;
    mx_status_t status;

    producer = mx_datapipe_create(0u, 1u, KB_(36), &consumer);
    ASSERT_GT(producer, 0, "could not create producer data pipe");
    ASSERT_GT(consumer, 0, "could not create consumer data pipe");

    // The writer goes faster, after 10 rounds the write cursor catches up from behind.
    for (int ix = 0; ; ++ix) {
        uintptr_t buffer = 0;
        mx_ssize_t avail = mx_datapipe_begin_write(producer, 0u, &buffer);
        if (avail < KB_(12)) {
            ASSERT_EQ(ix, 9, "bad cursor management");
            ASSERT_EQ(avail, KB_(9), "bad capacity");
            break;
        }

        memset((void*)buffer, ix, KB_(12));
        status = mx_datapipe_end_write(producer, KB_(12));
        ASSERT_EQ(status, NO_ERROR, "failed to end write");

        avail = mx_datapipe_begin_read(consumer, 0u, &buffer);
        ASSERT_GE(avail, KB_(9), "begin_read failed");
        status = mx_datapipe_end_read(consumer, KB_(9));
        ASSERT_EQ(status, NO_ERROR, "failed to end read");
    }

    status = mx_handle_close(consumer);
    ASSERT_GE(status, NO_ERROR, "failed to close data pipe");
    status = mx_handle_close(producer);
    ASSERT_GE(status, NO_ERROR, "failed to close data pipe");
    END_TEST;
}

static bool consumer_signals_when_producer_closed(void) {
    BEGIN_TEST;

    {
        mx_handle_t producer;
        mx_handle_t consumer;

        producer = mx_datapipe_create(0u, 1u, KB_(1), &consumer);
        ASSERT_GT(producer, 0, "could not create data pipe producer");
        ASSERT_GT(consumer, 0, "could not create data pipe consumer");

        ASSERT_EQ(mx_handle_close(producer), NO_ERROR, "failed to close data pipe producer");

        ASSERT_EQ(get_satisfied_signals(consumer), MX_SIGNAL_PEER_CLOSED,
                  "incorrect satisfied signals");
        ASSERT_EQ(get_satisfiable_signals(consumer), MX_SIGNAL_PEER_CLOSED,
                  "incorrect satisfiable signals");

        ASSERT_EQ(mx_handle_close(consumer), NO_ERROR, "failed to close data pipe consumer");
    }

    {
        mx_handle_t producer;
        mx_handle_t consumer;

        producer = mx_datapipe_create(0u, 1u, KB_(1), &consumer);
        ASSERT_GT(producer, 0, "could not create data pipe producer");
        ASSERT_GT(consumer, 0, "could not create data pipe consumer");

        ASSERT_EQ(mx_datapipe_write(producer, 0u, 10u, "0123456789"), 10, "write failed");

        ASSERT_EQ(mx_handle_close(producer), NO_ERROR, "failed to close data pipe producer");

        ASSERT_EQ(get_satisfied_signals(consumer), MX_SIGNAL_READABLE | MX_SIGNAL_PEER_CLOSED,
                  "incorrect satisfied signals");
        ASSERT_EQ(get_satisfiable_signals(consumer), MX_SIGNAL_READABLE | MX_SIGNAL_PEER_CLOSED,
                  "incorrect satisfiable signals");

        char buffer[64];
        ASSERT_EQ(mx_datapipe_read(consumer, 0u, 5, buffer), 5, "read failed");
        ASSERT_EQ(get_satisfied_signals(consumer), MX_SIGNAL_READABLE | MX_SIGNAL_PEER_CLOSED,
                  "incorrect satisfied signals");
        ASSERT_EQ(get_satisfiable_signals(consumer), MX_SIGNAL_READABLE | MX_SIGNAL_PEER_CLOSED,
                  "incorrect satisfiable signals");

        ASSERT_EQ(mx_datapipe_read(consumer, 0u, 5, buffer), 5, "read failed");
        ASSERT_EQ(get_satisfied_signals(consumer), MX_SIGNAL_PEER_CLOSED,
                  "incorrect satisfied signals");
        ASSERT_EQ(get_satisfiable_signals(consumer), MX_SIGNAL_PEER_CLOSED,
                  "incorrect satisfiable signals");

        ASSERT_EQ(mx_handle_close(consumer), NO_ERROR, "failed to close data pipe consumer");
    }

    END_TEST;
}

static bool nontrivial_element_size(void) {
    BEGIN_TEST;

    {
        mx_handle_t producer;
        mx_handle_t consumer;

        producer = mx_datapipe_create(0u, 5u, 125u, &consumer);
        ASSERT_GT(producer, 0, "could not create data pipe producer");
        ASSERT_GT(consumer, 0, "could not create data pipe consumer");

        EXPECT_EQ(mx_datapipe_write(producer, 0u, 5u, "01234"), 5, "write failed");
        EXPECT_EQ(mx_datapipe_write(producer, 0u, 10u, "0123456789"), 10, "write failed");

        uintptr_t ptr = 0u;
        mx_ssize_t avail = mx_datapipe_begin_write(producer, 0u, &ptr);
        ASSERT_EQ(avail, 110, "begin_write failed");
        memcpy((void*)ptr, "abcde", 5u);
        EXPECT_EQ(mx_datapipe_end_write(producer, 5u), NO_ERROR, "end_write failed");

        char buffer[100];
        EXPECT_EQ(mx_datapipe_read(consumer, 0u, 10u, buffer), 10, "read failed");
        EXPECT_EQ(memcmp(buffer, "0123401234", 10u), 0, "incorrect data from read");
        EXPECT_EQ(mx_datapipe_read(consumer, 0u, 5u, buffer), 5, "read failed");
        EXPECT_EQ(memcmp(buffer, "56789", 5u), 0, "incorrect data from read");

        ptr = 0u;
        avail = mx_datapipe_begin_read(consumer, 0u, &ptr);
        ASSERT_EQ(avail, 5, "begin_read failed");
        EXPECT_EQ(memcmp((const void*)ptr, "abcde", 5u), 0, "incorrect data from begin_read");
        EXPECT_EQ(mx_datapipe_end_read(consumer, 5u), NO_ERROR, "end_read_failed");

        EXPECT_EQ(mx_handle_close(producer), NO_ERROR, "failed to close data pipe producer");
        EXPECT_EQ(mx_handle_close(consumer), NO_ERROR, "failed to close data pipe consumer");
    }

    {
        mx_handle_t producer;
        mx_handle_t consumer;

        // Check that default capacity respects the element size. (Assume that the capacity is
        // reflected to the initial two-phase write.)
        producer = mx_datapipe_create(0u, 3u, 0u, &consumer);

        uintptr_t ptr = 0u;
        mx_ssize_t avail = mx_datapipe_begin_write(producer, 0u, &ptr);
        EXPECT_GT(avail, 0, "begin_write failed");
        EXPECT_EQ(avail % 3, 0, "invalid capacity");

        EXPECT_EQ(mx_handle_close(producer), NO_ERROR, "failed to close data pipe producer");
        EXPECT_EQ(mx_handle_close(consumer), NO_ERROR, "failed to close data pipe consumer");
    }

    END_TEST;
}

static bool element_size_errors(void) {
    BEGIN_TEST;

    {
        mx_handle_t unused;
        EXPECT_EQ(mx_datapipe_create(0u, 0u, 0u, &unused), ERR_INVALID_ARGS,
                  "create accepted invalid element size");
        EXPECT_EQ(mx_datapipe_create(0u, 2u, 3u, &unused), ERR_INVALID_ARGS,
                  "create accepted invalid capacity");
    }

    {
        mx_handle_t producer;
        mx_handle_t consumer;

        producer = mx_datapipe_create(0u, 5u, 0u, &consumer);
        ASSERT_GT(producer, 0, "could not create data pipe producer");
        ASSERT_GT(consumer, 0, "could not create data pipe consumer");

        EXPECT_EQ(mx_datapipe_write(producer, 0u, 4u, "0123"), ERR_INVALID_ARGS,
                  "write accepted invalid size?");

        uintptr_t ptr = 0u;
        mx_ssize_t avail = mx_datapipe_begin_write(producer, 0u, &ptr);
        ASSERT_GE(avail, 5, "begin_write failed");
        EXPECT_EQ(mx_datapipe_end_write(producer, 1u), ERR_INVALID_ARGS,
                  "end_write accepted invalid size?");
        // But it terminated the two-phase write anyway.
        EXPECT_EQ(mx_datapipe_end_write(producer, 0u), ERR_BAD_STATE,
                  "invalid end_write did not terminate two-phase write?");

        // Write some data, so we can reasonably test read errors.
        EXPECT_EQ(mx_datapipe_write(producer, 0u, 10u, "0123456789"), 10, "write failed");

        char buffer[100];
        EXPECT_EQ(mx_datapipe_read(consumer, 0u, 4u, buffer), ERR_INVALID_ARGS,
                  "read accepted invalid size?");

        ptr = 0u;
        avail = mx_datapipe_begin_read(consumer, 0u, &ptr);
        ASSERT_EQ(avail, 10, "begin_read failed");
        EXPECT_EQ(mx_datapipe_end_read(consumer, 4u), ERR_INVALID_ARGS,
                  "end_read accepted invalid size?");
        // But it terminated the two-phase read anyway.
        EXPECT_EQ(mx_datapipe_end_read(consumer, 0u), ERR_BAD_STATE,
                  "invalid end_read did not terminate two-phase read?");

        EXPECT_EQ(mx_handle_close(producer), NO_ERROR, "failed to close data pipe producer");
        EXPECT_EQ(mx_handle_close(consumer), NO_ERROR, "failed to close data pipe consumer");
    }

    END_TEST;
}

BEGIN_TEST_CASE(data_pipe_tests)
RUN_TEST(create_destroy_test)
RUN_TEST(simple_read_write)
RUN_TEST(loop_write_full)
RUN_TEST(write_read)
RUN_TEST(begin_write_read)
RUN_TEST(loop_write_read)
RUN_TEST(loop_begin_write_read)
RUN_TEST(consumer_signals_when_producer_closed)
RUN_TEST(nontrivial_element_size);
RUN_TEST(element_size_errors);
END_TEST_CASE(data_pipe_tests)

#ifndef BUILD_COMBINED_TESTS
int main(int argc, char** argv) {
    return unittest_run_all_tests(argc, argv) ? 0 : -1;
}
#endif
