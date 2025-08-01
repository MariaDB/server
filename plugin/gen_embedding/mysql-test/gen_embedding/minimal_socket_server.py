import socket
import json
import os
import sys
import signal


HOST = '127.0.0.1' # TODO: This should be configurable
PORT = int(sys.argv[1])
SUCCESS_RESPONSES_FILENAME = sys.argv[2]
WRONG_JSON_PATH_FILENAME = sys.argv[3]

with open(SUCCESS_RESPONSES_FILENAME, 'r') as file:
    responses = json.load(file)

def get_response(filename, status_code):
    if not os.path.exists(filename):
        print(f"File {filename} not found.")
        return build_response(json.dumps({"error": "File not found"}), 404)

    with open(filename, 'r') as file:
        content = file.read()

    return build_response(content, status_code, content_type="application/json")

def handle_request(request):
    try:
        request_line = request.splitlines()[0]
        method, path, _ = request_line.split()
        if path == "/success" and method in ("GET", "POST"):
            parsed_input = json.loads(request.splitlines()[-1])["input"]
            print(parsed_input)
            if parsed_input in responses:
                print(f"Input '{parsed_input}' found in {SUCCESS_RESPONSES_FILENAME}.")
                return_text = responses[parsed_input]
                return build_response(json.dumps(return_text), 200)
            else:
                print(f"Input '{parsed_input}' not found in {SUCCESS_RESPONSES_FILENAME}.")
                # In our tests we always provide a valid input, but we can return some form of error here
                return build_response(json.dumps({"error": "Not Found"}), 400)
        elif path == "/errorcode" and method in ("GET", "POST"):
            # The response body is irreleveant for this endpoint, the tests only care about the status code
            return build_response(json.dumps({"error": "Not Found"}), 400)
        elif path == "/wrongjsonpath" and method in ("GET", "POST"):
            return get_response(WRONG_JSON_PATH_FILENAME, 200)
        else:
            return build_response(json.dumps({"error": "Bad API endpoint or method"}), 404)

    except Exception as e:
        return build_response(json.dumps({"error": str(e)}), 500)

def build_response(body, status_code, content_type="application/json"):
    reason_phrases = {
        200: "OK",
        400: "Error",
        404: "Not Found"
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
    # print(response)
    return response.encode('utf-8')

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server_socket:
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server_socket.bind((HOST, PORT))
    except OSError as e:
        print(f"Error binding to port {PORT}: {e}")
        sys.exit(0)
    server_socket.listen(1)

    print(f"Started mockup API server on http://{HOST}:{PORT} ...")

    while True:
        client_connection, client_address = server_socket.accept()
        with client_connection:
            request = client_connection.recv(4096).decode('utf-8')
            if request:
                print("Received request:")
                print(request)
                response = handle_request(request)
                client_connection.sendall(response)
