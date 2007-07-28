require 'mkmf'
require 'pkg-config'

def find_smjs(mozjs)
  dir_config(mozjs)
  #$CFLAGS += " -gdbg"
  case RUBY_PLATFORM
  when /mswin32|mingw|bccwin32/
    $defs << " -DXP_WIN"
    lib = "js32"
  else
    $defs << " -DXP_UNIX"
    lib = mozjs
  end

  $defs << " -DNEED_#{mozjs.upcase}_PREFIX"
  have_library(lib)
end

if %w(xulrunner-js thunderbird-js mozilla-js).any? do |package|
    PKGConfig.have_package(package)
  end or find_smjs('mozjs') or find_smjs('smjs')
  create_makefile("spidermonkey")
else
  exit 1
end
