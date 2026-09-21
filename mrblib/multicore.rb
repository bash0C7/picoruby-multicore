# Multicore: run a kernel on another core by name.
#
#   Multicore.run(:scale_sum, [1, 2, 3], 10)      # => 60
#   job = Multicore.spawn(:scale_sum, [1, 2, 3], 10)
#   job.done?                                     # never blocks
#   job.value                                     # => 60
#   Multicore.open { |mc| mc.run(:dbl, [1.5]) }   # releases the core on the way out
#
# The kernels are native functions a build registers in a table (see
# include/multicore.h). This layer turns the arguments into MessagePack bytes,
# hands them to the worker, and turns the reply back into a Ruby value. The C
# glue never raises; every failure becomes a Multicore::Error here.
#
# Written for both VMs, so it stays inside the mruby/c subset: no
# defined?, Hash#fetch, inline rescue, proc, lambda, String#unpack, Class.new,
# Array#concat or **opts, while loops, and explicit requires.
class Multicore
  class Error < StandardError
  end
  class CoreBusy < Error
  end
  class UnknownKernel < Error
  end
  class QueueFull < Error
  end
  class InputTooLarge < Error
  end
  class OutputTooLarge < Error
  end
  class TypeError < Error
  end
  class RangeError < Error
  end
  class KernelError < Error
  end
  class Timeout < Error
  end

  DEFAULT_TIMEOUT_MS = 10_000

  # Value tree -> MessagePack. Float is always float64, Symbol is a str, Hash is a map.
  class Encoder
    def initialize(mc)
      @mc = mc
      @buf = ""
    end

    def bytes
      @buf
    end

    def byte(n)
      @buf << n.chr
    end

    def uint(n, width)
      i = width - 1
      while i >= 0
        byte((n >> (8 * i)) & 0xff)
        i -= 1
      end
    end

    def head(n, fix, wide16, wide32)
      if n < 16
        byte(fix | n)
      elsif n < 65536
        byte(wide16)
        uint(n, 2)
      else
        byte(wide32)
        uint(n, 4)
      end
    end

    def int(v)
      if v >= 0 && v <= 127
        byte(v)
      elsif v < 0 && v >= -32
        byte(v & 0xff)
      elsif v >= -128 && v <= 127
        byte(0xd0)
        uint(v, 1)
      elsif v >= -32768 && v <= 32767
        byte(0xd1)
        uint(v, 2)
      elsif v >= -2147483648 && v <= 2147483647
        byte(0xd2)
        uint(v, 4)
      else
        byte(0xd3)
        uint(v, 8)
      end
    end

    def str(s)
      n = s.bytesize
      if n < 32
        byte(0xa0 | n)
      elsif n < 256
        byte(0xd9)
        uint(n, 1)
      elsif n < 65536
        byte(0xda)
        uint(n, 2)
      else
        byte(0xdb)
        uint(n, 4)
      end
      @buf << s
    end

    def array(a)
      head(a.length, 0x90, 0xdc, 0xdd)
      i = 0
      while i < a.length
        encode(a[i])
        i += 1
      end
    end

    def hash(h)
      ks = h.keys
      head(ks.length, 0x80, 0xde, 0xdf)
      i = 0
      while i < ks.length
        encode(ks[i])
        encode(h[ks[i]])
        i += 1
      end
    end

    def encode(obj)
      if obj.nil?
        byte(0xc0)
      elsif obj == true
        byte(0xc3)
      elsif obj == false
        byte(0xc2)
      elsif obj.is_a?(Integer)
        int(obj)
      elsif obj.is_a?(Float)
        byte(0xcb)
        @buf << @mc._f2s(obj)
      elsif obj.is_a?(String)
        str(obj)
      elsif obj.is_a?(Symbol)
        str(obj.to_s)
      elsif obj.is_a?(Array)
        array(obj)
      elsif obj.is_a?(Hash)
        hash(obj)
      else
        raise Multicore::TypeError, "a #{obj.class} cannot be passed to a kernel"
      end
    end
  end

  # MessagePack -> value tree. Reads float32 as well as float64; str and bin
  # both come back as a String, map keys as they are on the wire (a Symbol
  # arrives as a String).
  class Decoder
    def initialize(mc, bytes)
      @mc = mc
      @s = bytes
      @i = 0
    end

    def finished?
      @i == @s.bytesize
    end

    def need(n)
      if @i + n > @s.bytesize
        raise Multicore::Error, "the kernel's reply is cut short"
      end
    end

    def uint(n)
      need(n)
      v = 0
      k = 0
      while k < n
        v = (v << 8) | @s.getbyte(@i)
        @i += 1
        k += 1
      end
      v
    end

    def sint(n)
      v = uint(n)
      if v >= (1 << (8 * n - 1))
        v -= (1 << (8 * n))
      end
      v
    end

    def int64(signed)
      hi = uint(4)
      lo = uint(4)
      if hi >= 0x80000000
        raise Multicore::RangeError, "the kernel returned an Integer outside the Integer range" unless signed
        hi -= 0x100000000
      end
      (hi << 32) | lo
    end

    def str(n)
      need(n)
      s = @s.byteslice(@i, n)
      @i += n
      s
    end

    def float(n)
      need(n)
      f = @mc._s2f(@s.byteslice(@i, n))
      @i += n
      f
    end

    def array(n)
      a = []
      i = 0
      while i < n
        a << read
        i += 1
      end
      a
    end

    def hash(n)
      h = {}
      i = 0
      while i < n
        k = read
        h[k] = read
        i += 1
      end
      h
    end

    def read
      need(1)
      t = @s.getbyte(@i)
      @i += 1
      if t <= 0x7f
        t
      elsif t >= 0xe0
        t - 256
      elsif t >= 0xa0 && t <= 0xbf
        str(t & 0x1f)
      elsif t >= 0x90 && t <= 0x9f
        array(t & 0x0f)
      elsif t >= 0x80 && t <= 0x8f
        hash(t & 0x0f)
      elsif t == 0xc0
        nil
      elsif t == 0xc2
        false
      elsif t == 0xc3
        true
      elsif t == 0xca
        float(4)
      elsif t == 0xcb
        float(8)
      elsif t == 0xcc
        uint(1)
      elsif t == 0xcd
        uint(2)
      elsif t == 0xce
        uint(4)
      elsif t == 0xcf
        int64(false)
      elsif t == 0xd0
        sint(1)
      elsif t == 0xd1
        sint(2)
      elsif t == 0xd2
        sint(4)
      elsif t == 0xd3
        int64(true)
      elsif t == 0xd9 || t == 0xc4
        str(uint(1))
      elsif t == 0xda || t == 0xc5
        str(uint(2))
      elsif t == 0xdb || t == 0xc6
        str(uint(4))
      elsif t == 0xdc
        array(uint(2))
      elsif t == 0xdd
        array(uint(4))
      elsif t == 0xde
        hash(uint(2))
      elsif t == 0xdf
        hash(uint(4))
      else
        raise Multicore::Error, "the kernel's reply holds an unsupported MessagePack type"
      end
    end
  end

  # One call of a kernel. Its slot on the worker is held until the result is
  # collected, so call done? or value on every job you spawn.
  class Job
    def initialize(mc, id, name)
      @mc = mc
      @id = id
      @name = name
      @finished = false
      @value = nil
      @error = nil
    end

    # Never blocks. True once the kernel has finished (or the job is gone).
    def done?
      return true if @finished
      st = @mc._poll(@id)
      return false if st == 0
      if st == 1
        collect
      else
        @finished = true
        @error = Multicore::Error.new("the job of #{@name} is gone: the worker was closed")
      end
      true
    end

    # Blocks until the kernel has finished. Raises Multicore::Timeout when it
    # has not within timeout_ms (nil waits without a limit); the job goes on
    # and can be waited for again.
    def wait(timeout_ms: Multicore::DEFAULT_TIMEOUT_MS)
      return self if done?
      started = @mc._now_ms
      spins = 0
      while !done?
        if timeout_ms && ((@mc._now_ms - started) & 0xffffffff) >= timeout_ms
          raise Multicore::Timeout, "#{@name} did not finish within #{timeout_ms} ms"
        end
        spins += 1
        sleep_ms(1) if spins > 50
      end
      self
    end

    # The kernel's result as a Ruby value, or the exception it maps to.
    def value(timeout_ms: Multicore::DEFAULT_TIMEOUT_MS)
      wait(timeout_ms: timeout_ms)
      raise @error if @error
      @value
    end

    # Give up on a job whose result is no longer wanted. Its slot is freed as
    # soon as the worker is through with it.
    def discard
      unless @finished
        @mc._forget(@id)
        @finished = true
        @error = Multicore::Error.new("the job of #{@name} was discarded")
      end
      nil
    end

    def collect
      r = @mc._result(@id)
      @finished = true
      status = r[0]
      data = r[1]
      if status >= 0
        begin
          d = Multicore::Decoder.new(@mc, data)
          @value = d.read
          unless d.finished?
            @error = Multicore::Error.new("the reply of #{@name} has bytes left over")
          end
        rescue => e
          @error = e
        end
      else
        @error = @mc._error_for(status, @name, data)
      end
    end
  end

  # The one worker of the process is shared, so the class methods forward to a
  # single instance.
  def self.core
    @core ||= Multicore.new
  end

  def self.run(name, *args)
    core.run(name, *args)
  end

  def self.spawn(name, *args)
    core.spawn(name, *args)
  end

  def self.running?
    core._running
  end

  def self.close
    core.close
  end

  def self.in_cap
    core._limits[0]
  end

  def self.out_cap
    core._limits[1]
  end

  def self.queue_depth
    core._limits[2]
  end

  # Takes the core, yields the worker, and releases the core however the block ends.
  def self.open
    mc = core
    mc._ensure_started
    begin
      yield mc
    ensure
      mc.close
    end
  end

  # run(name, *args, timeout_ms: 10_000): call the kernel and wait for its result.
  # timeout_ms is read from a trailing {timeout_ms: n} Hash, because mruby/c
  # cannot tell a Hash argument from keywords when a method also takes *args.
  def run(name, *args)
    timeout_ms = DEFAULT_TIMEOUT_MS
    if args.length > 0
      last = args[args.length - 1]
      if last.is_a?(Hash) && last.length == 1 && last.keys[0] == :timeout_ms
        timeout_ms = last[:timeout_ms]
        args.pop
      end
    end
    job = spawn(name, *args)
    begin
      job.value(timeout_ms: timeout_ms)
    rescue Multicore::Timeout => e
      job.discard
      raise e
    end
  end

  # Queue the call and return a Job at once.
  def spawn(name, *args)
    n = name.to_s
    if _signature(n).nil?
      names = _names
      listed = names.empty? ? "no kernel is registered" : "registered: #{names}"
      raise Multicore::UnknownKernel, "unknown kernel #{n} (#{listed})"
    end
    enc = Multicore::Encoder.new(self)
    enc.array(args)
    bytes = enc.bytes
    if bytes.bytesize > _limits[0]
      raise Multicore::InputTooLarge, "the arguments of #{n} take #{bytes.bytesize} bytes; the input buffer holds #{_limits[0]}"
    end
    _ensure_started
    id = _submit(n, bytes)
    if id < 0
      if id == -4
        raise Multicore::QueueFull, "#{_limits[2]} jobs are already queued or waiting to be collected"
      elsif id == -3
        raise Multicore::InputTooLarge, "the arguments of #{n} do not fit the input buffer"
      elsif id == -2
        raise Multicore::UnknownKernel, "unknown kernel #{n}"
      else
        raise Multicore::Error, "the worker is not running"
      end
    end
    Multicore::Job.new(self, id, n)
  end

  # Stop the worker and release the core. Jobs still queued are dropped.
  def close
    unless _stop
      raise Multicore::Timeout, "the worker is still inside a kernel; call close again later"
    end
    nil
  end

  def _ensure_started
    return nil if _running
    st = _start
    if st == 2
      raise Multicore::CoreBusy, "the second core is in use by something else (picoruby-psg?)"
    elsif st != 0
      raise Multicore::Error, "the worker could not be started"
    end
    nil
  end

  def _error_for(status, name, data)
    if status == -1
      Multicore::TypeError.new("the arguments of #{name} do not match its signature #{_signature(name)}")
    elsif status == -2
      Multicore::OutputTooLarge.new("the result of #{name} does not fit the #{_limits[1]} byte output buffer")
    elsif status == -3
      Multicore::KernelError.new("#{name}: #{data}")
    elsif status == -4
      Multicore::RangeError.new("an Integer argument of #{name} does not fit the kernel's Integer width")
    else
      Multicore::Error.new("#{name} returned an unknown status")
    end
  end
end
