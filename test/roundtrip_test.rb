# Every type crosses to the kernel and back. The :echo kernel returns the
# bytes it was given, so what these tests see is what the Ruby layer encodes
# and decodes. Runs on both VMs (mruby and mruby/c), so it sticks to the mruby/c subset.
class RoundtripTest < Picotest::Test
  def teardown
    Multicore.close
  end

  def echo(v)
    Multicore.run(:echo, v)
  end

  def bytes_of(s)
    a = []
    i = 0
    while i < s.bytesize
      a << s.getbyte(i)
      i += 1
    end
    a
  end

  def encoded(v)
    e = Multicore::Encoder.new(Multicore.core)
    e.encode(v)
    bytes_of(e.bytes)
  end

  def decoded(bytes)
    s = ""
    bytes.each { |b| s << b.chr }
    Multicore::Decoder.new(Multicore.core, s).read
  end

  def bits(f)
    bytes_of(Multicore.core._f2s(f))
  end

  def test_the_encoder_writes_the_msgpack_wire_format
    assert_equal [0xc0], encoded(nil)
    assert_equal [0xc3], encoded(true)
    assert_equal [0xc2], encoded(false)
    assert_equal [0x00], encoded(0)
    assert_equal [0x7f], encoded(127)
    assert_equal [0xff], encoded(-1)
    assert_equal [0xe0], encoded(-32)
    assert_equal [0xd0, 0xdf], encoded(-33)
    assert_equal [0xd0, 0x80], encoded(-128)
    assert_equal [0xd1, 0x00, 0x80], encoded(128)
    assert_equal [0xd1, 0x01, 0x2c], encoded(300)
    assert_equal [0xd2, 0x00, 0x01, 0x00, 0x00], encoded(65536)
    assert_equal [0xd3, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00], encoded(1 << 32)
    assert_equal [0xd3, 0xff, 0xff, 0xff, 0xfe, 0xff, 0xff, 0xff, 0xff], encoded(-(1 << 32) - 1)
    assert_equal [0xcb, 0x3f, 0xf8, 0, 0, 0, 0, 0, 0], encoded(1.5)
    assert_equal [0xa0], encoded("")
    assert_equal [0xa1, 0x61], encoded("a")
    assert_equal [0xa3, 0x61, 0x62, 0x63], encoded(:abc)
    assert_equal [0x91, 0x01], encoded([1])
    assert_equal [0x81, 0xa1, 0x61, 0x01], encoded({"a" => 1})
  end

  def test_the_decoder_reads_every_msgpack_form_a_kernel_may_answer_with
    assert_equal 255, decoded([0xcc, 0xff])
    assert_equal 65535, decoded([0xcd, 0xff, 0xff])
    assert_equal 4294967295, decoded([0xce, 0xff, 0xff, 0xff, 0xff])
    assert_equal(-2, decoded([0xd0, 0xfe]))
    assert_equal(-2, decoded([0xd1, 0xff, 0xfe]))
    assert_equal(-2, decoded([0xd2, 0xff, 0xff, 0xff, 0xfe]))
    assert_equal(-2, decoded([0xd3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe]))
    assert_equal((1 << 40) + 5, decoded([0xd3, 0, 0, 1, 0, 0, 0, 0, 5]))
    assert_equal 1.5, decoded([0xca, 0x3f, 0xc0, 0x00, 0x00])
    assert_equal 1.5, decoded([0xcb, 0x3f, 0xf8, 0, 0, 0, 0, 0, 0])
    assert_equal "ab", decoded([0xc4, 0x02, 0x61, 0x62])
    assert_equal "ab", decoded([0xd9, 0x02, 0x61, 0x62])
    assert_equal [1, 2], decoded([0xdc, 0x00, 0x02, 0x01, 0x02])
    assert_equal({"a" => 1}, decoded([0xde, 0x00, 0x01, 0xa1, 0x61, 0x01]))
  end

  def test_integers_survive_at_every_width
    [0, 1, 127, 128, 255, 256, 32767, 32768, 65535, 65536, -1, -32, -33, -128, -129,
     -32768, -32769, 2147483647, 2147483648, -2147483648, -2147483649,
     (1 << 40) + 5, -(1 << 40) - 5, 1 << 62].each do |v|
      assert_equal v, echo(v)
    end
  end

  def test_the_largest_and_smallest_integer
    max = 9223372036854775807
    assert_equal max, echo(max)
    min = -9223372036854775807 - 1
    assert_equal min, echo(min)
  end

  def test_floats_come_back_bit_for_bit
    nan_bytes = ""
    [0x7f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00].each { |n| nan_bytes << n.chr }
    nan = Multicore.core._s2f(nan_bytes)
    inf = 1.0 / 0.0
    [1.5, -2.25, 0.1, 1e300, -1e-300, inf, -inf, -0.0, 0.0, 5.0e-324, 2.2250738585072014e-308].each do |f|
      assert_equal bits(f), bits(echo(f))
    end
    # The wire keeps a NaN's bytes, but mruby stamps a serial number into the payload of every
    # NaN the VM makes, so only the NaN pattern itself (sign, exponent, quiet bit) is compared.
    # `nan` is built from an explicit bit pattern, not `0.0 / 0.0`: the sign a CPU gives that
    # division's result is architecture-defined (ARM's default NaN has it clear, x86's
    # "indefinite" NaN has it set), which made this assertion depend on the host's CPU.
    r = echo(nan)
    assert r != r
    b = bits(r)
    assert_equal 0x7f, b[0]
    assert_equal 0xf8, b[1] & 0xf8
    assert_equal 1.0 / 0.0, echo(inf)
    assert_equal(-1.0 / 0.0, 1.0 / echo(-0.0))
    assert_equal 5.0e-324, echo(5.0e-324)
  end

  def test_a_float32_reply_is_read
    assert_equal 1.5, Multicore.run(:f32)
  end

  def test_strings
    ["", "hello", "日本語のテキスト", "a" * 31, "a" * 32, "b" * 255, "c" * 256, "d" * 3000].each do |s|
      assert_equal s, echo(s)
    end
    nul = "a"
    nul << 0.chr
    nul << "b"
    r = echo(nul)
    assert_equal 3, r.bytesize
    assert_equal 0, r.getbyte(1)
  end

  def test_a_string_of_all_byte_values_is_not_altered
    s = ""
    i = 0
    while i < 256
      s << i.chr
      i += 1
    end
    r = echo(s)
    assert_equal 256, r.bytesize
    assert_equal bytes_of(s), bytes_of(r)
  end

  def test_symbols_are_sent_as_strings
    assert_equal "abc", echo(:abc)
    assert_equal({"a" => 1, "b" => 2}, echo({a: 1, b: 2}))
  end

  def test_nil_true_and_false
    assert_nil echo(nil)
    assert_equal true, echo(true)
    assert_equal false, echo(false)
  end

  def test_arrays_and_hashes_nest
    assert_equal [], echo([])
    assert_equal({}, echo({}))
    v = [1, [2, [3, []]], "x", nil]
    assert_equal v, echo(v)
    h = {"a" => {"b" => [1, 2, {"c" => nil}]}, "d" => 2.5}
    assert_equal h, echo(h)
    twenty = []
    i = 0
    while i < 20
      twenty << i
      i += 1
    end
    assert_equal twenty, echo(twenty)
    big = {}
    twenty.each { |n| big["k#{n}"] = n }
    assert_equal big, echo(big)
  end

  def test_hash_order_is_kept
    h = {}
    h["z"] = 1
    h["a"] = 2
    h["m"] = 3
    assert_equal ["z", "a", "m"], echo(h).keys
  end

  def test_several_arguments
    assert_equal 5, Multicore.run(:add, 2, 3)
    assert_equal(-7 + (1 << 40), Multicore.run(:add, -7, 1 << 40))
    assert_equal 60, Multicore.run(:scale_sum, [1, 2, 3], 10)
    assert_equal [[2], "日本", 1], Multicore.run(:reverse, [1, "日本", [2]])
    assert_equal({"a" => 2, "b" => 3}, Multicore.run(:bump, {"a" => 1, "b" => 2}))
    d = Multicore.run(:dbl, [1.5, -0.0, 0.25])
    assert_equal 3.0, d[0]
    assert_equal bits(-0.0), bits(d[1])
    assert_equal 0.5, d[2]
  end

  def test_a_result_that_nearly_fills_the_output_buffer
    r = Multicore.run(:bigstr, Multicore.out_cap - 3)
    assert_equal Multicore.out_cap - 3, r.bytesize
  end
end
