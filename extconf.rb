require 'mkmf'
dir_config("smjs")
#$CFLAGS += " -gdbg"
case RUBY_PLATFORM
when /mswin32|mingw|bccwin32/
	$defs << "-DXP_WIN"
	lib = "js32"
else
	$defs << "-DXP_UNIX"
	lib = "smjs"
end
#if have_header("smjs/jsapi.h") or have_header("jsapi.h")
if have_library(lib)
	create_makefile("spidermonkey")
end
#end
