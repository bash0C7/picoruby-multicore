# The wire carries a Symbol as a str. The Ruby layer reads the return type of
# the called kernel's signature and turns Strings into Symbols exactly where
# the type says Symbol; every other String stays a String. The fake kernels
# echo their argument and only differ in the signature they declare.
class SymbolsTest < Picotest::Test
  def teardown
    Multicore.close
  end

  def types(sig)
    Multicore::Types.for_signature(sig)
  end

  def test_the_walker_finds_the_symbol_positions_of_the_return_type
    assert_equal [:sym], types("() -> Symbol")
    assert_equal [:sym], types("(Integer) -> Symbol")
    assert_equal [:sym], types("  ( Integer ) ->  Symbol ? ")
    assert_equal [:array, [:sym]], types("(Integer) -> Array[Symbol]")
    assert_equal [:array, [:sym]], types("(Integer) -> Array[Symbol]?")
    assert_equal [:hash, [:sym], nil], types("(Array[Symbol]) -> Hash[Symbol, Float]")
    assert_equal [:hash, nil, [:sym]], types("(Integer) -> Hash[String, Symbol]")
    assert_equal [:hash, [:sym], [:sym]], types("(Integer) -> Hash[Symbol,Symbol]")
    assert_equal [:tuple, [[:sym], nil, nil]], types("(untyped) -> [Symbol, String, Integer]")
    assert_equal [:array, [:hash, [:sym], [:array, [:sym]]]], types("(untyped) -> Array[Hash[Symbol, Array[Symbol]]]")
    assert_equal [:sym], types("(Integer) -> ::Symbol")
  end

  def test_the_walker_finds_nothing_where_the_return_type_has_no_symbol
    assert_nil types("(Integer) -> Integer")
    assert_nil types("(Symbol, Array[Symbol]) -> Integer")
    assert_nil types("(Hash[String, Symbol]) -> Array[String]")
    assert_nil types("(untyped) -> untyped")
    assert_nil types("(Integer) -> void")
    assert_nil types("(Integer) -> Time")
    assert_nil types("(Integer) -> Hash[String, Integer]")
    assert_nil types("no arrow here")
    assert_nil types("(Integer) -> Array[")
    assert_nil types("")
  end

  def test_convert_touches_only_the_marked_positions
    tree = types("(untyped) -> Hash[Symbol, Array[String]]")
    assert_nil types("(untyped) -> Hash[String, Array[String]]")
    r = Multicore::Types.convert({"a" => ["x"]}, tree)
    assert r.keys[0].is_a?(Symbol)
    assert r[:a][0].is_a?(String)
    same = {"a" => "b"}
    assert_equal same, Multicore::Types.convert(same, nil)
  end

  def test_a_symbol_comes_back_as_a_symbol
    r = Multicore.run(:sym, :abc)
    assert r.is_a?(Symbol)
    assert_equal :abc, r
    assert_equal :abc, Multicore.run(:sym, "abc")
  end

  def test_array_of_symbols
    r = Multicore.run(:syms, [:a, :b])
    assert_equal [:a, :b], r
    assert r[0].is_a?(Symbol)
    assert_equal [], Multicore.run(:syms, [])
  end

  def test_hash_with_symbol_keys
    r = Multicore.run(:symhash, {a: 1.5, b: 2.5})
    assert_equal({a: 1.5, b: 2.5}, r)
    assert r.keys[0].is_a?(Symbol)
    assert r[:a].is_a?(Float)
  end

  def test_hash_with_symbol_values_keeps_string_keys
    r = Multicore.run(:symvals, {"k" => :x})
    assert r.keys[0].is_a?(String)
    assert_equal :x, r["k"]
    assert r["k"].is_a?(Symbol)
  end

  def test_symbol_keys_and_symbol_values
    r = Multicore.run(:symsym, {a: :b})
    assert_equal({a: :b}, r)
  end

  def test_hash_with_symbol_keys_and_string_values_keeps_the_values_as_strings
    r = Multicore.run(:symkeys_strvals, {a: "s"})
    assert r.keys[0].is_a?(Symbol)
    assert r[:a].is_a?(String)
    assert_equal "s", r[:a]
  end

  def test_tuple
    r = Multicore.run(:tuple, [:a, "b", 3])
    assert_equal [:a, "b", 3], r
    assert r[0].is_a?(Symbol)
    assert r[1].is_a?(String)
  end

  def test_optional_symbol
    assert_nil Multicore.run(:optsym, nil)
    assert_equal :a, Multicore.run(:optsym, :a)
  end

  def test_nested
    r = Multicore.run(:nested, [{a: [:x, :y]}, {b: []}])
    assert_equal [{a: [:x, :y]}, {b: []}], r
    assert r[0][:a][1].is_a?(Symbol)
  end

  def test_strings_stay_strings_where_the_type_says_string
    r = Multicore.run(:strs, ["a", "b"])
    assert_equal ["a", "b"], r
    assert r[0].is_a?(String)
    h = Multicore.run(:strhash, {"a" => 1})
    assert h.keys[0].is_a?(String)
  end

  def test_untyped_and_unmarked_kernels_leave_strings_alone
    assert Multicore.run(:poly, "abc").is_a?(String)
    assert Multicore.run(:echo, {"a" => ["b"]}).keys[0].is_a?(String)
    assert Multicore.run(:echo, :abc).is_a?(String)
  end
end
