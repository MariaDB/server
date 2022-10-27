module Groonga
  class ScanInfoSearchIndex < Struct.new(:index_column,
                                         :section_id,
                                         :weight,
                                         :scorer,
                                         :scorer_args_expr,
                                         :scorer_args_expr_offset)
  end
end
