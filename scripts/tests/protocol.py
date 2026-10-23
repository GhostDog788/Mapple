import subprocess,threading,queue,json,time
class RPC:
    def __init__(self,argv,stderr):
        self.p=subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=stderr)
        self.messages=queue.Queue();self.pending=[];self.seq=0
        threading.Thread(target=self.reader,daemon=True).start()
    def reader(self):
        try:
            while True:
                headers={}
                while True:
                    line=self.p.stdout.readline()
                    if not line:return
                    if line in (b'\r\n',b'\n'):break
                    k,v=line.decode().split(':',1);headers[k.lower()]=v.strip()
                data=self.p.stdout.read(int(headers['content-length']))
                self.messages.put(json.loads(data))
        finally:self.messages.put({'closed':True})
    def send(self,msg):
        self.seq+=1
        if msg.get('type')=='request':msg['seq']=self.seq
        body=json.dumps(msg).encode();self.p.stdin.write(f'Content-Length: {len(body)}\r\n\r\n'.encode()+body);self.p.stdin.flush()
        return self.seq
    def wait(self,predicate,timeout=40):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            for i,m in enumerate(self.pending):
                if predicate(m):return self.pending.pop(i)
            m=self.messages.get(timeout=max(.1,end-time.monotonic()))
            if m.get('closed'):raise RuntimeError('RPC closed')
            if predicate(m):return m
            self.pending.append(m)
        raise TimeoutError('RPC timeout')
    def lsp(self,method,params,request=True):
        msg={'jsonrpc':'2.0','method':method,'params':params}
        if request:
            self.seq+=1;msg['id']=self.seq
        self.send(msg)
        if request:
            response=self.wait(lambda m:m.get('id')==msg['id']);assert 'error' not in response,response
            return response.get('result')
    def dap(self,command,args,wait=True):
        seq=self.send({'type':'request','command':command,'arguments':args})
        if not wait:return seq
        return self.response(seq)
    def response(self,seq):
        m=self.wait(lambda m:m.get('type')=='response' and m.get('request_seq')==seq,60)
        assert m.get('success'),m
        return m.get('body',{})
