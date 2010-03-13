require("rpc")

function error_handler (message)
	io.write ("Err: " .. message .. "\n");
end

print('connect')
if rpc.mode == "tcpip" then
    slave, err = rpc.connect ("localhost",12346);
elseif rpc.mode == "serial" then
    slave, err = rpc.connect ("/dev/ttys0");
end
print('done')

-- Local Dataset

tab = {a=1, b=2};

test_local = {1, 2, 3, 4, "234"}
test_local.sval = 23

function squareval(x) return x*x end

--
-- BEGIN TESTS
--
function myassert(...)
  print('do')
  print('do')
  assert(...)
  print('done')
end

for i=1,1 do

-- check that our connection exists
assert( slave, "connection failed" )

-- reflect parameters off mirror
-- print("Sending 42")
print('do'); assert(slave.mirror(42) == 42, "integer return failed")
-- print("Done 42")
-- print(slave.mirror("012345673901234")) -- why the heck does this fail for things of length 15 (16 w/ null)?
-- slave.mirror("01234567890123456789012")
print('do'); assert(slave.mirror("The quick brown fox jumps over the lazy dog") == "The quick brown fox jumps over the lazy dog", "string return failed")

-- print(slave.mirror(squareval))
print('do'); assert(slave.mirror(true) == true, "function return failed")

-- basic remote call with returned data
print('do'); assert( slave.foo1 (123,56,"hello") == 456, "basic call and return failed" )

-- execute function remotely
print('do'); assert(slave.execfunc( string.dump(squareval), 8 ) == 64, "couldn't serialize and execute dumped function")

-- get remote table
print('do'); assert(slave.test:get(), "couldn't get remote table")

-- check that we can get entry on remote table
print('do'); assert(test_local.sval == slave.test:get().sval, "table field not equivalent")

print('set')
slave.yarg.blurg = 23
print('done')
print('do'); assert(slave.yarg.blurg:get() == 23, "not equal")

-- function assigment
print('set')
slave.squareval = squareval
print('done')
print('do'); assert(type(slave.squareval) == "userdata", "function assigment failed")

-- remote execution of assigned function
print('do'); assert(slave.squareval(99) == squareval(99), "remote setting and evaluation of function failed")
print('next')
end

-- ensure that we're not loosing critical objects in GC
tval = 5
y={}
y.z={}
y.z.x = tval
print('set')
slave.y=y
print('done')

a={}
for i=1,2 do
  print('get')
  a[i]=slave.y.z
  collectgarbage("collect")
end
for idx,val in ipairs(a) do
  print('do'); assert(val:get().x == tval, "missing parent helper")
  print('do'); assert(val.x:get() == tval, "missing parent helper")
end

s=("The quick brown fox jumps over the lazy dog"):rep(100000)
print('do'); assert(slave.mirror(s) == s, "huge string return failed")

rpc.close (slave)
