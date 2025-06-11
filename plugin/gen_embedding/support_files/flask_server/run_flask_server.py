from flask import Flask, request, jsonify
import sys
import json

app = Flask(__name__)
filename = ""


@app.route('/embeddings', methods=['GET','POST'])
def get_item():
    with open(filename, 'r') as file:
        embeddings = json.load(file) 
    return jsonify(embeddings)

def main():
    global filename
    filename = sys.argv[1]
    app.run(debug=True)

if __name__ == '__main__':
    main()
