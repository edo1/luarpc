require("udp")

a=rpc_transport.udp()

a:connect("127.0.0.1",12345)
s=("hi"):rep(32000);

for i=1,1000 do
  a:write(s)
  s1=a:read()
end

print(s1:len())
a:close()
a=nil
