require 'mkmf'
require 'pkg-config'

$defs << " -Wall"

%w(xulrunner-js thunderbird-js mozilla-js).any? do |package|
  PKGConfig.have_package(package)
end

have_func("JS_SetOperationCallback")
have_func("JS_SetBranchCallback")

create_makefile("spidermonkey")
