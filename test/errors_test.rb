# Each failure has its own exception class, all under Multicore::Error.
class ErrorsTest < Picotest::Test
  def teardown
    Multicore.close
  end

  def failure
    yield
    nil
  rescue => e
    e
  end

  def test_unknown_kernel_names_the_kernel_and_lists_the_registered_ones
    e = failure { Multicore.run(:no_such_kernel) }
    assert e.is_a?(Multicore::UnknownKernel)
    assert e.is_a?(Multicore::Error)
    assert e.message.include?("no_such_kernel")
    assert e.message.include?("echo")
    assert e.message.include?("scale_sum")
    assert_false Multicore.running?
  end

  def test_a_kernel_that_rejects_its_arguments_raises_type_error
    e = failure { Multicore.run(:malformed) }
    assert e.is_a?(Multicore::TypeError)
    assert e.message.include?("malformed")
    assert e.is_a?(Multicore::Error)
    assert failure { Multicore.run(:add, "a", 1) }.is_a?(Multicore::TypeError)
    assert failure { Multicore.run(:add, 1) }.is_a?(Multicore::TypeError)
  end

  def test_a_value_the_wire_cannot_carry_raises_type_error_and_does_not_start_the_worker
    e = failure { Multicore.run(:echo, Object.new) }
    assert e.is_a?(Multicore::TypeError)
    assert_false Multicore.running?
  end

  def test_a_result_too_big_raises_output_too_large
    assert failure { Multicore.run(:nospace) }.is_a?(Multicore::OutputTooLarge)
    assert failure { Multicore.run(:bigstr, Multicore.out_cap) }.is_a?(Multicore::OutputTooLarge)
  end

  def test_a_kernel_exception_raises_kernel_error_with_its_message
    e = failure { Multicore.run(:boom) }
    assert e.is_a?(Multicore::KernelError)
    assert e.message.include?("RuntimeError: bang")
    assert e.message.include?("boom")
  end

  def test_an_integer_out_of_range_raises_range_error
    assert failure { Multicore.run(:range) }.is_a?(Multicore::RangeError)
  end

  def test_an_unknown_status_is_still_a_multicore_error
    e = failure { Multicore.run(:weird) }
    assert e.is_a?(Multicore::Error)
    assert_equal Multicore::Error, e.class
  end

  def test_arguments_that_do_not_fit_the_input_buffer_raise_input_too_large
    e = failure { Multicore.run(:echo, "x" * (Multicore.in_cap + 1)) }
    assert e.is_a?(Multicore::InputTooLarge)
    assert e.is_a?(Multicore::Error)
    assert_equal "x" * 1000, Multicore.run(:echo, "x" * 1000)
  end

  def test_the_worker_survives_a_failing_kernel
    assert failure { Multicore.run(:boom) }.is_a?(Multicore::KernelError)
    assert_equal 5, Multicore.run(:add, 2, 3)
  end

  def test_the_exceptions_are_all_under_error
    [Multicore::CoreBusy, Multicore::UnknownKernel, Multicore::QueueFull, Multicore::InputTooLarge,
     Multicore::OutputTooLarge, Multicore::TypeError, Multicore::RangeError, Multicore::KernelError,
     Multicore::Timeout].each do |k|
      assert k.new("m").is_a?(Multicore::Error)
    end
    assert Multicore::Error.new("m").is_a?(StandardError)
  end
end
