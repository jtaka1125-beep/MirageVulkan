import socket, struct, os
HOST='0.0.0.0'
PORT=int(os.environ.get('AI_PORT','51100'))
OUT=os.environ.get('AI_OUT', r'C:\MirageWork\ops_snapshots\ai_latest.jpg')

srv=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
srv.bind((HOST,PORT))
srv.listen(1)
print(f'listening {HOST}:{PORT}')
conn,addr=srv.accept()
print('conn',addr)

def recvn(n):
    b=b''
    while len(b)<n:
        c=conn.recv(n-len(b))
        if not c: raise EOFError
        b+=c
    return b

hdr=recvn(4+4+4+8)
ln,w,h,ts=struct.unpack('!iiiq',hdr)
jpeg=recvn(ln)
with open(OUT,'wb') as f:
    f.write(jpeg)
print('got frame',w,h,'len',ln,'tsUs',ts,'->',OUT)
conn.close(); srv.close()
