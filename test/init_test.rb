# A kernel's init runs once, before that kernel's first call.
class InitTest < Picotest::Test
  def teardown
    Multicore.close
  end

  def test_init_runs_once_per_kernel_before_its_first_call
    assert_equal 1, Multicore.run(:init_calls)
    assert_equal 1, Multicore.run(:init_calls)
    assert_equal 3, Multicore.run(:add, 1, 2)
    assert_equal 2, Multicore.run(:init_calls)
    Multicore.close
    assert_equal 3, Multicore.run(:add, 1, 2)
    assert_equal 2, Multicore.run(:init_calls)
  end
end
