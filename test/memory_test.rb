# The job slots are heap-allocated while the worker runs and freed when it has stopped.
# The host port counts its live allocations (:live_allocs answers it from the worker).
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

  def test_the_slots_are_the_only_allocation_and_a_start_close_cycle_leaks_nothing
    assert_equal 1, Multicore.run(:live_allocs)
    i = 0
    while i < 25
      Multicore.close
      assert_equal 1, Multicore.run(:live_allocs)
      i += 1
    end
  end

  def test_a_failed_allocation_raises_no_memory_and_leaves_the_worker_stopped
    ENV["MULTICORE_HOST_FAIL_ALLOC"] = "1"
    e = failure { Multicore.run(:add, 1, 2) }
    assert e.is_a?(Multicore::NoMemory)
    assert e.is_a?(Multicore::Error)
    assert_false Multicore.running?
    ENV["MULTICORE_HOST_FAIL_ALLOC"] = "0"
    assert_equal 3, Multicore.run(:add, 1, 2)
    assert_equal 1, Multicore.run(:live_allocs)
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
    assert_equal 1, Multicore.run(:live_allocs)
  end

  def test_the_queue_depth_default_is_four_and_the_caps_are_4096
    assert_equal 4, Multicore.queue_depth
    assert_equal 4096, Multicore.in_cap
    assert_equal 4096, Multicore.out_cap
  end
end
