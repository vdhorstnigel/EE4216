import socket
import struct
import cv2
import numpy as np

HOST = "0.0.0.0"  # listen on all interfaces
PORT = 5050       # must match firmware MOTION_TCP_PORT


def recv_exact(conn, n):
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def serve():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen(5)
        print(f"Listening on {HOST}:{PORT} ...")
        while True:
            conn, addr = s.accept()
            with conn:
                print(f"Connection from {addr}")
                # Protocol used by firmware: [4 bytes big-endian length][JPEG bytes]
                hdr = recv_exact(conn, 4)
                if not hdr:
                    print("Client closed before length header")
                    continue
                (length,) = struct.unpack("!I", hdr)
                img_bytes = recv_exact(conn, length)
                if not img_bytes:
                    print("Client closed before image bytes")
                    continue
                # Decode JPEG to numpy array and show
                arr = np.frombuffer(img_bytes, dtype=np.uint8)
                img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
                if img is None:
                    print("Failed to decode JPEG")
                else:
                    cv2.imshow("ESP Image", img)
                    cv2.waitKey(1)
                # one-shot connection per image; firmware closes after send


if __name__ == "__main__":
    serve()
