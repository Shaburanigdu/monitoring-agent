from http.server import BaseHTTPRequestHandler, HTTPServer
import json

class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length)
        try:
            data = json.loads(body)
            print("Received:", json.dumps(data, ensure_ascii=False, indent=2))
        except Exception as e:
            print("Invalid JSON:", e)
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'{"status":"ok"}')

if __name__ == '__main__':
    server = HTTPServer(('127.0.0.1', 8080), Handler)
    print("Demo server on http://localhost:8080")
    server.serve_forever()