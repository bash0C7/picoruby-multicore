# Multicore: 別の core で走るネイティブの worker と、Integer のスカラーだけをやりとりする。
# VM は自分の core に残り、worker は VM に触らない。
#
#   mc = Multicore.new(unit: :core1_lcg)
#   mc.send(300_000)       # すぐ戻る
#   r = mc.take            # 結果が無ければ nil
#   mc.close
#
# unit 名から worker への対応は ports/<board>/multicore.c が持つ (I2C.new(unit:) と同じ作り)。
# この Ruby 層は名前の表を持たず、文字列を C へ渡すだけ。
class Multicore
  class Error < StandardError
  end

  JOB_MAX = 2147483647

  def initialize(unit:)
    status = _open(unit.to_s)
    if status == 1
      raise Error, "Multicore has no worker for unit #{unit.to_s} on this board"
    elsif status == 2
      raise Error, "Multicore cannot take the core for unit #{unit.to_s}: it is already in use"
    elsif status != 0
      raise Error, "Multicore could not start the worker for unit #{unit.to_s}"
    end
    @open = true
  end

  # ノンブロッキング。job を queue に積んで nil を返す
  def send(value)
    raise Error, "Multicore is closed" unless @open
    raise Error, "Multicore#send takes an Integer" unless value.is_a?(Integer)
    raise Error, "Multicore#send takes 0..#{JOB_MAX}" if value < 0 || JOB_MAX < value
    raise Error, "Multicore input queue is full" unless _send(value)
    nil
  end

  # ノンブロッキング。結果が届いていなければ nil
  def take
    return nil unless @open
    _take
  end

  def close
    return nil unless @open
    raise Error, "Multicore worker is still busy; close again later" unless _close
    @open = false
    nil
  end
end
