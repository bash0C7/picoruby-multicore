# spawn / done? / value, the queue, timeouts, and the life cycle of the worker.
class JobsTest < Picotest::Test
  def teardown
    Multicore.close
  end

  def failure
    yield
    nil
  rescue => e
    e
  end

  def test_the_worker_starts_on_the_first_call_and_can_be_closed_and_started_again
    assert_false Multicore.running?
    assert_equal 5, Multicore.run(:add, 2, 3)
    assert Multicore.running?
    assert_nil Multicore.close
    assert_false Multicore.running?
    assert_equal 9, Multicore.run(:add, 4, 5)
    assert Multicore.running?
    Multicore.close
    Multicore.close
    assert_false Multicore.running?
  end

  def test_spawn_returns_at_once_and_value_waits
    job = Multicore.spawn(:scale_sum, [1, 2, 3], 10)
    assert_equal 60, job.value
    assert_equal 60, job.value
    assert job.done?
  end

  def test_done_does_not_block
    job = Multicore.spawn(:slow, 200)
    t0 = Multicore.core._now_ms
    finished = job.done?
    elapsed = Multicore.core._now_ms - t0
    assert_false finished
    assert elapsed < 100
    assert_nil job.value
    assert job.done?
  end

  def test_wait_returns_the_job
    job = Multicore.spawn(:slow, 20)
    assert_equal job, job.wait
    assert job.done?
  end

  def test_several_jobs_run_in_order_and_each_result_belongs_to_its_job
    first = Multicore.spawn(:stamp)
    slow = Multicore.spawn(:slow, 30)
    second = Multicore.spawn(:stamp)
    add1 = Multicore.spawn(:add, 1, 1)
    add2 = Multicore.spawn(:add, 2, 2)
    add3 = Multicore.spawn(:add, 3, 3)
    assert_equal 6, add3.value
    assert_equal 4, add2.value
    assert_equal 2, add1.value
    b = second.value
    a = first.value
    assert_equal a + 1, b
    assert_nil slow.value
  end

  def test_the_queue_holds_queue_depth_jobs
    depth = Multicore.queue_depth
    jobs = []
    depth.times { jobs << Multicore.spawn(:slow, 20) }
    e = failure { Multicore.spawn(:add, 1, 1) }
    assert e.is_a?(Multicore::QueueFull)
    jobs.each { |j| j.value }
    assert_equal 2, Multicore.run(:add, 1, 1)
  end

  def test_run_times_out_and_the_worker_goes_on
    e = failure { Multicore.run(:slow, 300, timeout_ms: 30) }
    assert e.is_a?(Multicore::Timeout)
    assert e.is_a?(Multicore::Error)
    assert_equal 3, Multicore.run(:add, 1, 2)
  end

  def test_a_timed_out_run_gives_its_slot_back
    depth = Multicore.queue_depth
    failure { Multicore.run(:slow, 100, timeout_ms: 10) }
    assert_equal 2, Multicore.run(:add, 1, 1)
    jobs = []
    depth.times { jobs << Multicore.spawn(:add, 1, 1) }
    jobs.each { |j| assert_equal 2, j.value }
  end

  def test_a_job_can_be_waited_for_again_after_a_timeout
    job = Multicore.spawn(:slow, 150)
    e = failure { job.value(timeout_ms: 10) }
    assert e.is_a?(Multicore::Timeout)
    assert_nil job.value
  end

  def test_no_timeout_waits_for_the_result
    assert_nil Multicore.run(:slow, 50, timeout_ms: nil)
  end

  def test_discard_frees_the_slot
    depth = Multicore.queue_depth
    job = Multicore.spawn(:slow, 60)
    assert_nil job.discard
    assert_equal 2, Multicore.run(:add, 1, 1)
    jobs = []
    depth.times { jobs << Multicore.spawn(:add, 1, 1) }
    jobs.each { |j| j.value }
    assert failure { job.value }.is_a?(Multicore::Error)
  end

  def test_open_yields_the_worker_and_releases_the_core
    r = Multicore.open { |mc| mc.run(:add, 20, 22) }
    assert_equal 42, r
    assert_false Multicore.running?
    assert_equal [3.0], Multicore.open { |mc| mc.run(:dbl, [1.5]) }
  end

  def test_open_starts_the_worker_before_the_block
    Multicore.open do |mc|
      assert Multicore.running?
    end
  end

  def test_open_releases_the_core_when_the_block_raises
    e = failure { Multicore.open { |mc| mc.run(:add, 1, 2); raise "oops" } }
    assert_equal "oops", e.message
    assert_false Multicore.running?
    assert_equal 3, Multicore.open { |mc| mc.run(:add, 1, 2) }
  end

  def test_a_job_outlived_by_the_worker_raises_an_error
    job = Multicore.spawn(:slow, 40)
    Multicore.close
    assert job.done?
    assert failure { job.value }.is_a?(Multicore::Error)
  end

  def test_the_core_busy_status_raises_core_busy
    ENV["MULTICORE_HOST_CORE_BUSY"] = "1"
    e = failure { Multicore.run(:add, 1, 2) }
    ENV["MULTICORE_HOST_CORE_BUSY"] = "0"
    assert e.is_a?(Multicore::CoreBusy)
    assert_false Multicore.running?
  end
end
