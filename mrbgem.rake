MRuby::Gem::Specification.new('picoruby-multicore') do |spec|
  spec.license = 'MIT'
  spec.author  = 'bash0C7'
  spec.summary = 'Run kernels on another core by name: Multicore.run(:name, args...)'

  spec.require_name = 'multicore'

  spec.cc.include_paths << "#{dir}/include"

  # ports/<board>/multicore.c needs the board SDK's include paths, so the
  # board's firmware build definition compiles it (ESP-IDF SRCS on esp32, the
  # CMake source list on rp2040). The kernel table comes from the build that
  # owns the kernels (sendairk03's picoruby-kernel_registry).
  #
  # The host build has neither: it compiles the pthread port and a hand-written
  # table of fake kernels (test/support) into libmruby, which is what the host
  # tests run. Kernels never reach a board build this way.
  if build.posix?
    spec.cc.defines << 'MULTICORE_TEST_TABLE'
    spec.linker.libraries << 'pthread'
    %w[ports/host/multicore.c test/support/fake_kernels.c].each do |rel|
      spec.objs << "#{dir}/#{rel}".relative_path_from(dir).pathmap("#{build_dir}/%X.o")
    end
  end
end
