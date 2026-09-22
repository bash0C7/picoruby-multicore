# The job slots are heap-allocated while the worker runs and freed when it has stopped.
# The host port counts its live allocations (:live_allocs answers it from the worker). The worker
# holds 1 + 2 * queue depth blocks: the slot array and one input and one output buffer per slot.
class MemoryTest < Picotest::Test
  def teardown
    ENV["MULTICORE_HOST_FAIL_ALLOC"] = "0"
    Multicore.close
  end

  def failure
    yield
    nil
  rescue => e
    e
  end

  def blocks
    1 + 2 * Multicore.queue_depth
  end

  def test_a_start_close_cycle_leaks_nothing
    assert_equal blocks, Multicore.run(:live_allocs)
    i = 0
    while i < 25
      Multicore.close
      assert_equal blocks, Multicore.run(:live_allocs)
      i += 1
    end
  end

  # MULTICORE_HOST_FAIL_ALLOC=n fails the n-th allocation of the next start. Failing each one in turn
  # (the first, the middle ones, the last) must leave nothing behind: the block count afterwards
  # is exactly that of a clean start.
  def test_a_failed_allocation_raises_no_memory_and_leaves_nothing_behind
    n = 1
    while n <= blocks
      ENV["MULTICORE_HOST_FAIL_ALLOC"] = n.to_s
      e = failure { Multicore.run(:add, 1, 2) }
      assert e.is_a?(Multicore::NoMemory)
      assert e.is_a?(Multicore::Error)
      assert_false Multicore.running?
      n += 1
    end
    ENV["MULTICORE_HOST_FAIL_ALLOC"] = "0"
    assert_equal 3, Multicore.run(:add, 1, 2)
    assert_equal blocks, Multicore.run(:live_allocs)
  end

  def test_close_while_a_kernel_runs_keeps_the_memory_and_reports_busy
    job = Multicore.spawn(:slow, 2400)
    sleep_ms(100)
    e = failure { Multicore.close }
    assert e.is_a?(Multicore::Timeout)
    assert_false Multicore.running?
    assert failure { Multicore.run(:add, 1, 2) }.is_a?(Multicore::CoreBusy)
    sleep_ms(700)
    assert_equal 3, Multicore.run(:add, 1, 2)
    assert_equal blocks, Multicore.run(:live_allocs)
  end

  def test_the_queue_depth_default_is_four_and_the_caps_are_4096
    assert_equal 4, Multicore.queue_depth
    assert_equal 4096, Multicore.in_cap
    assert_equal 4096, Multicore.out_cap
  end
end
