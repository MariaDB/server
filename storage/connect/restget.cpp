/************* Restget C++ Program Source Code File (.CPP) *************/
/* Adapted from the sample program of the Casablanca tutorial.         */
/* Copyright Olivier Bertrand 2019.                                    */
/***********************************************************************/
#include <cpprest/filestream.h>
#include <cpprest/http_client.h>
#if defined(MARIADB)
#include <my_global.h>
#else
#include "mini-global.h"
#define _OS_H_INCLUDED     // Prevent os.h to be called
#endif

using namespace utility::conversions; // String conversions utilities
using namespace web;                  // Common features like URIs.
using namespace web::http;            // Common HTTP functionality
using namespace web::http::client;    // HTTP client features
using namespace concurrency::streams; // Asynchronous streams

#include "global.h"

/***********************************************************************/
/*  Make a local copy of the requested file.                           */
/***********************************************************************/
int restGetFile(PGLOBAL g, PCSZ http, PCSZ uri, PCSZ fn)
{
  int  rc = 0;
	bool xt = trace(515);
  auto fileStream = std::make_shared<ostream>();

  if (!http || !fn) {
    strcpy(g->Message, "Missing http or filename");
    return 2;
  } // endif

	if (xt)
	  htrc("restGetFile: fn=%s\n", fn);

  // Open stream to output file.
  pplx::task<void> requestTask = fstream::open_ostream(to_string_t(fn))
    .then([=](ostream outFile) {
      *fileStream= outFile;

			if (xt)
				htrc("Outfile isopen=%d\n", outFile.is_open());

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
				htrc("Received response status code:%u\n",
                       response.status_code());

      // Write response body into the file.
      return response.body().read_to_end(fileStream->streambuf());
    })

    // Close the file stream.
    .then([=](size_t n) {
			if (xt)
			  htrc("Return size=%u\n", n);

      return fileStream->close();
    });

  // Wait for all the outstanding I/O to complete and handle any exceptions
  try {
    requestTask.wait();

		if (xt)
      htrc("In Wait\n");

  } catch (const std::exception &e) {
		if (xt)
		  htrc("Error exception: %s\n", e.what());
    sprintf(g->Message, "Error exception: %s", e.what());
    rc= 1;
  } // end try/catch

	if (xt)
	  htrc("restget done: rc=%d\n", rc);

  return rc;
} // end of restGetFile
