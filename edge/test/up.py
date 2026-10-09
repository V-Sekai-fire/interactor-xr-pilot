# Fake MCP upstream: POST /mcp echoes the request id after DELAY seconds.
import json, os, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class H(BaseHTTPRequestHandler):
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        time.sleep(float(os.environ.get('DELAY', '0')))
        if 'id' not in req:
            self.send_response(202)
            self.end_headers()
            return
        body = json.dumps({"jsonrpc": "2.0", "id": req['id'], "result": {"echo": req.get('method')}}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
