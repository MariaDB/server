This directory contains support files for the gen_embedding plugin

The `json_responses` directory contains some responses from OpenAI's API that can be used for testing.

The `flask_server` directory contains a minimal Flask server, to be used as a mockup server for API testing instead of the actual OpenAI endpoint.

Requirements:
- python3, tested with Python 3.12.3 but most versions should work
- A (virtual) environment with the packages from `flask_server/requirements.txt` installed

Usage:

The `run_flask_server.py` script starts the Flask server.
It takes a single command line argument, the path of the JSON file containing the response. The server will always return the contents of the file provided, as it is a minimal server for testing and implements no additional logic besides that.
The server by default listens to port `5000`.

For example, to start a server that returns the contents of `openai_response_ada_002.json` (provided in the `json_responses` directory):

`python3 run_flask_server.py PATH/TO/SERVER/plugin/gen_embedding/support_files/json_responses/openai_response_ada_002.json`