import socket, struct, time, os

HOST='0.0.0.0'
PORT=int(os.environ.get('AI_PORT','51100'))
OUT=os.environ.get('AI_OUT', r'C:\MirageWork\ops_snapshots\ai_latest.jpg')

print(f'listen {HOST}:{PORT} -> {OUT}')

srv=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
srv.bind((HOST,PORT))
srv.listen(1)

conn,addr=srv.accept()
print('conn from',addr)
conn.settimeout(10)

def recvn(n):
    b=b''
    while len(b)<n:
        chunk=conn.recv(n-len(b))
        if not chunk:
            raise EOFError
        b+=chunk
    return b

while True:
    try:
        hdr=recvn(4+4+4+8)
        ln,w,h,ts=struct.unpack('!iiiq',hdr)
        jpeg=recvn(ln)
        with open(OUT,'wb') as f:
            f.write(jpeg)
        print(time.strftime('%H:%M:%S'), 'frame', w,h,'len',ln,'tsUs',ts)
    except Exception as e:
        print('err',e)
        break
