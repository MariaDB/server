function dictionarySource(url) {
    function displayItems(items) {
        var results = $("<dl />");
        $.each(items,
	       function(i, val) {
		   results.append($("<dt />")
				  .append($("<span />")
					  .text(val[0])
					  .click(function() {
						     $(".search").val($(this).text());
						     $("#search").submit();
						 })));
		   results.append($("<dd />")
				  .append($("<span />").text(val[1]))
				  .append($("<span />").text(val[2]))
				 );
	       });
        $("#result")
	    .empty()
	    .append(results);
    };

    var request_index = 0;
    var columns = "_key,gene95_desc,edict_desc";
    var xhr;
    function source(request, response) {
	function onSuccess(data, status) {
	    if (this.autocomplete_request != request_index) {
		return;
	    }
	    var completions = data[1]["complete"];
	    var items = [];
	    if (completions && completions.length > 2) {
		completions.shift();
		completions.shift();
		$.each(completions,
		       function(i, item) {
			   var key = item[0];
			   items.push(key);
			   if (items.length >= 3) {
			       return false;
			   }
			   return true;
		       });
	    }
	    if (completions.length > 0) {
		displayItems(completions);
	    }
	    response(items);
	}

	function onError() {
	    if (this.autocomplete_request != request_index) {
		return;
	    }
	    response([]);
	}

	if (xhr) {
	    xhr.abort();
	}
	xhr = $.ajax(url,
		     {
			 data: {
			     query: request.term,
			     types: 'complete',
			     table: 'item_dictionary',
			     column: 'kana',
			     limit: 25,
			     output_columns: columns,
			     frequency_threshold: 1,
			     prefix_search: "yes"
			 },
			 dataType: "jsonp",
			 autocomplete_request: ++request_index,
			 success: onSuccess,
			 error: onError
		     });
    };

    return source;
}
