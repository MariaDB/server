/************* Restget C++ Program Source Code File (.CPP) *************/
/* Adapted from the sample program of the Casablanca tutorial.         */
/* Copyright Olivier Bertrand 2019.                                    */
/***********************************************************************/
#include <cpprest/filestream.h>
#include <cpprest/http_client.h>

using namespace utility::conversions; // String conversions utilities
using namespace web;                  // Common features like URIs.
using namespace web::http;            // Common HTTP functionality
using namespace web::http::client;    // HTTP client features
using namespace concurrency::streams; // Asynchronous streams

typedef const char* PCSZ;

extern "C" int restGetFile(char* m, bool xt, PCSZ http, PCSZ uri, PCSZ fn);

/***********************************************************************/
/*  Make a local copy of the requested file.                           */
/***********************************************************************/
int restGetFile(char *m, bool xt, PCSZ http, PCSZ uri, PCSZ fn)
{
  int  rc = 0;
  auto fileStream = std::make_shared<ostream>();

  if (!http || !fn) {
    //strcpy(g->Message, "Missing http or filename");
		strcpy(m, "Missing http or filename");
		return 2;
  } // endif

	if (xt)
		fprintf(stderr, "restGetFile: fn=%s\n", fn);

  // Open stream to output file.
  pplx::task<void> requestTask = fstream::open_ostream(to_string_t(fn))
    .then([=](ostream outFile) {
      *fileStream= outFile;

			if (xt)
				fprintf(stderr, "Outfile isopen=%d\n", outFile.is_open());

      // Create http_client to send the request.
      http_client client(to_string_t(http));

      if (uri) {
        // Build request URI and start the request.
        uri_builder builder(to_string_t(uri));
        return client.request(methods::GET, builder.to_string());
      } else
        return client.request(methods::GET);
    })

    // Handle response headers arriving.
    .then([=](http_response response) {
			if (xt)
				fprintf(stderr, "Received response status code:%u\n",
                                  response.status_code());

      // Write response body into the file.
      return response.body().read_to_end(fileStream->streambuf());
    })

    // Close the file stream.
    .then([=](size_t n) {
			if (xt)
				fprintf(stderr, "Return size=%zu\n", n);

      return fileStream->close();
    });

  // Wait for all the outstanding I/O to complete and handle any exceptions
  try {
		if (xt)
			fprintf(stderr, "Waiting\n");

		requestTask.wait();
  } catch (const std::exception &e) {
		if (xt)
			fprintf(stderr, "Error exception: %s\n", e.what());

		sprintf(m, "Error exception: %s", e.what());
		rc= 1;
  } // end try/catch

	if (xt)
		fprintf(stderr, "restget done: rc=%d\n", rc);

  return rc;
} // end of restGetFile
