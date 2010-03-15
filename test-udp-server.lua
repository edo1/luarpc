require("udp")

a=rpc_transport.udp()

a:listen(12345)

while (1) do
  s=a:read()
  a:write("reply to "..s)
end
a:close()
a=nil
