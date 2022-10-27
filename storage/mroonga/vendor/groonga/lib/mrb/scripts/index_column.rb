module Groonga
  class IndexColumn
    private :estimate_size_for_term_id
    private :estimate_size_for_query
    private :estimate_size_for_lexicon_cursor

    # Estimate the number of matched records for term ID or query.
    #
    # @overload estimate_size(:term_id => term_id)
    #   @return [Integer] the number of matched records for the term ID.
    #
    # @overload estimate_size(:query => query)
    #   @return [Integer] the number of matched records for the query.
    #
    # @overload estimate_size(:lexicon_cursor => lexicon_cursor)
    #   @return [Integer] the number of matched records for the lexicon cursor.
    #
    def estimate_size(parameters)
      term_id = parameters[:term_id]
      if term_id
        return estimate_size_for_term_id(term_id)
      end

      query = parameters[:query]
      if query
        return estimate_size_for_query(query, parameters)
      end

      lexicon_cursor = parameters[:lexicon_cursor]
      if lexicon_cursor
        return estimate_size_for_lexicon_cursor(lexicon_cursor)
      end

      message =
        "must specify :term_id, :query, :lexicon_cursor: #{parameters.inspect}"
      raise InvalidArgument, message
    end
  end
end
