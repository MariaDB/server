import socket
import json
import os
import sys
import signal


HOST = '127.0.0.1' # TODO: This should be configurable
PORT = int(sys.argv[1])
FILENAME = sys.argv[2]
RETURN_CODE = int(sys.argv[3])


def handle_request(request):
    try:
        request_line = request.splitlines()[0]
        method, path, _ = request_line.split()
        
        if path == "/embeddings" and method in ("GET", "POST"):
            if not os.path.exists(FILENAME):
                print(FILENAME)
                response_body = json.dumps({"error": "File not found"})
                return build_response(response_body, 404)

            with open(FILENAME, 'r') as file:
                embeddings = json.load(file)

            response_body = json.dumps(embeddings)
            return build_response(response_body, RETURN_CODE, content_type="application/json")
        else:
            return build_response(json.dumps({"error": "Not Found"}), 404)

    except Exception as e:
        return build_response(json.dumps({"error": str(e)}), 500)

def build_response(body, status_code, content_type="application/json"):
    reason_phrases = {
        200: "OK",
        404: "Not Found",
        404: "Other Not Found",
        500: "Internal Server Error"
    }
    status_text = reason_phrases.get(status_code, "Unknown")
    response = (
        f"HTTP/1.1 {status_code} {status_text}\r\n"
        f"Content-Type: {content_type}\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n"
        "\r\n"
        f"{body}"
    )
    print(response)
    return response.encode('utf-8')

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server_socket:
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_socket.bind((HOST, PORT))
    server_socket.listen(1)

    print(f"Serving '/embeddings' on http://{HOST}:{PORT} ...")

    while True:
        client_connection, client_address = server_socket.accept()
        with client_connection:
            request = client_connection.recv(4096).decode('utf-8')
            if request:
                print("Received request:")
                print(request)
                response = handle_request(request)
                client_connection.sendall(response)
