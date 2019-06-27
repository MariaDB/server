/************* Restget C++ Program Source Code File (.CPP) *************/
/* Adapted from the sample program of the Casablanca tutorial.         */
/* Copyright Olivier Bertrand 2019.                                    */
/***********************************************************************/
#include <cpprest/filestream.h>
#include <cpprest/http_client.h>
#if defined(MARIADB)
#include <my_global.h>
#else
#include "mini_global.h"
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
  int  rc= 0;
  auto fileStream= std::make_shared<ostream>();

	if (!http || !fn) {
		strcpy(g->Message, "Missing http or filename");
		return 2;
	} // endif

  //std::string sfn(fn);
  //auto wfn= to_string_t(sfn);
  //rc= 0;

  // Open stream to output file.
  pplx::task<void> requestTask=
      fstream::open_ostream(to_string_t(fn))
          .then([=](ostream outFile) {
            *fileStream= outFile;

            // Create http_client to send the request.
            http_client client(to_string_t(http));

            if (uri)
            {
              // Build request URI and start the request.
              uri_builder builder(to_string_t(uri));
              return client.request(methods::GET, builder.to_string());
            }
            else
              return client.request(methods::GET);
          })

          // Handle response headers arriving.
          .then([=](http_response response) {
#if defined(DEVELOPMENT)
						fprintf(stderr, "Received response status code:%u\n",
                    response.status_code());
#endif   // DEVELOPMENT

            // Write response body into the file.
            return response.body().read_to_end(fileStream->streambuf());
          })

          // Close the file stream.
          .then([=](size_t) { return fileStream->close(); });

  // Wait for all the outstanding I/O to complete and handle any exceptions
  try
  {
    requestTask.wait();
  }
  catch (const std::exception &e)
  {
#if defined(DEVELOPMENT)
		fprintf(stderr, "Error exception: %s\n", e.what());
#endif   // DEVELOPMENT
		sprintf(g->Message, "Error exception: %s", e.what());
    rc= 1;
  } // end try/catch

  return rc;
} // end of restGetFile