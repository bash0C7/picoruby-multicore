MRuby::Gem::Specification.new('picoruby-multicore') do |spec|
  spec.license = 'MIT'
  spec.author  = 'bash0C7'
  spec.summary = 'Run a native kernel on another core and exchange Integer scalars with Ruby'

  spec.require_name = 'multicore'

  spec.cc.include_paths << "#{dir}/include"

  # ports/<board>/multicore.c needs the board SDK's include paths, so the
  # board's firmware build definition compiles it (ESP-IDF SRCS on esp32, the
  # CMake source list on rp2040). The host port has no such dependency and is
  # compiled into libmruby here, which is what the host tests run.
  if build.posix?
    src = "#{dir}/ports/host/multicore.c"
    spec.objs << src.relative_path_from(dir).pathmap("#{build_dir}/%X.o")
  end
end
