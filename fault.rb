require 'date'
require 'spidermonkey'

class RubyObj
  def foo(bar)
    bar.to_s
  end
  def quux(xyzzy)
    [xyzzy]
  end
  def fred(barney)
    [barney]
  end
end

cx = SpiderMonkey::Context.new
cx.set_property('obj', RubyObj.new)
cx.set_property('out', STDOUT)

cx.eval(<<-JS)

var ary = [];
for (var i = 0; i < 100000; i++) {
  out.puts(i);
  var a = obj.foo("a" + i);
  var b = obj.fred(a);
  var c = obj.fred(b);
  ary.push([a, b, c]);
}

JS

cx.gc

puts "Okay!"

