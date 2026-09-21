# host port (unit :host_lcg) を両方の VM で回す。worker は process にひとつなので、
# どの test も自分で open して自分で close する。
class MulticoreTest < Picotest::Test
  def test_unknown_unit_raises_an_error
    raised = nil
    begin
      Multicore.new(unit: :no_such_worker)
    rescue Multicore::Error => e
      raised = e
    end
    assert_not_nil raised
    assert raised.message.include?("no_such_worker")
  end

  def test_send_and_take_carry_one_integer
    mc = Multicore.new(unit: :host_lcg)
    assert_nil mc.take
    assert_nil mc.send(1000)
    assert_equal 1219259225, mc.take
    assert_nil mc.take
    assert_nil mc.close
  end

  def test_the_kernel_gives_the_documented_values
    mc = Multicore.new(unit: :host_lcg)
    mc.send(0)
    assert_equal 1, mc.take
    mc.send(1)
    assert_equal 1103527590, mc.take
    mc.send(2)
    assert_equal 377401575, mc.take
    mc.send(10)
    assert_equal 267834847, mc.take
    mc.send(300000)
    assert_equal 680765729, mc.take
    mc.close
  end

  def test_send_takes_an_integer_only
    mc = Multicore.new(unit: :host_lcg)
    assert_raise(Multicore::Error) { mc.send("1000") }
    assert_raise(Multicore::Error) { mc.send(nil) }
    assert_raise(Multicore::Error) { mc.send(:gate_on) }
    assert_raise(Multicore::Error) { mc.send([1, 2]) }
    assert_raise(Multicore::Error) { mc.send(-1) }
    assert_nil mc.take
    mc.close
  end

  def test_a_closed_worker_refuses_send_and_takes_nothing
    mc = Multicore.new(unit: :host_lcg)
    mc.close
    assert_raise(Multicore::Error) { mc.send(10) }
    assert_nil mc.take
    assert_nil mc.close
  end

  def test_the_core_is_taken_by_one_worker_at_a_time
    mc = Multicore.new(unit: :host_lcg)
    raised = nil
    begin
      Multicore.new(unit: :host_lcg)
    rescue Multicore::Error => e
      raised = e
    end
    assert_not_nil raised
    assert raised.message.include?("already in use")
    mc.close
    second = Multicore.new(unit: :host_lcg)
    second.send(2)
    assert_equal 377401575, second.take
    second.close
  end
end
