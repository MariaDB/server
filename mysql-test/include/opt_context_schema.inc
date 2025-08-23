let $opt_context_schema=
{
  "description": "schema corresponding to the optimizer context section of the trace",
  "type": "object",
  "properties": {
    "current_database": {
      "type": "string",
      "minLength": 1
    },
    "list_contexts": {
      "type": "array",
      "uniqueItems": true,
      "minItems": 1,
      "items": {
        "type": "object",
        "properties": {
          "name": {
            "type": "string",
            "minLength": 1
          },
          "ddl": {
            "type": "string",
            "minLength": 1
          },
          "num_of_records": {
            "type": "number"
          },
          "read_cost_io": {
            "type": "number"
          },
          "read_cost_cpu": {
            "type": "number"
          },
          "indexes": {
            "type": "array",
            "uniqueItems": true,
            "minItems": 1,
            "items": {
              "type": "object",
              "properties": {
                "index_name": {
                  "type": "string",
                  "minLength": 1
                },
                "rec_per_key": {
                  "type": "array",
                  "items": {
                    "type": "string"
                  }
                }
              },
              "required": [
                "index_name",
                "rec_per_key"
              ]
            }
          },
          "list_ranges": {
            "type": "array",
            "uniqueItems": true,
            "minItems": 1,
            "items": {
              "type": "object",
              "properties": {
                "index_name": {
                  "type": "string",
                  "minLength": 1
                },
                "ranges": {
                  "type": "array",
                  "items": {
                    "type": "string"
                  }
                },
                "num_rows": {
                  "type": "number"
                },
                "cost": {
                  "type": "object",
                  "properties": {
                    "avg_io_cost": {
                      "type": "number"
                    },
                    "cpu_cost": {
                      "type": "number"
                    },
                    "comp_cost": {
                      "type": "number"
                    },
                    "copy_cost": {
                      "type": "number"
                    },
                    "limit_cost": {
                      "type": "number"
                    },
                    "setup_cost": {
                      "type": "number"
                    },
                    "index_cost_io": {
                      "type": "number"
                    },
                    "index_cost_cpu": {
                      "type": "number"
                    },
                    "row_cost_io": {
                      "type": "number"
                    },
                    "row_cost_cpu": {
                      "type": "number"
                    }
                  },
                  "required": [
                    "avg_io_cost",
                    "cpu_cost",
                    "comp_cost",
                    "copy_cost",
                    "limit_cost",
                    "setup_cost",
                    "index_cost_io",
                    "index_cost_cpu",
                    "row_cost_io",
                    "row_cost_cpu"
                  ]
                },
                "max_index_blocks": {
                  "type": "number"
                },
                "max_row_blocks": {
                  "type": "number"
                }
              },
              "required": [
                "index_name",
                "ranges",
                "num_rows",
                "cost",
                "max_index_blocks",
                "max_row_blocks"
              ]
            }
          },
          "list_index_read_costs": {
            "type": "array",
            "uniqueItems": false,
            "minItems": 1,
            "items": {
              "type": "object",
              "properties": {
                "key_number": {
                  "type": "number"
                },
                "num_records": {
                  "type": "number"
                },
                "eq_ref": {
                  "type": "number",
                  "minimum": 0,
                  "maximum": 1
                },
                "index_cost_io": {
                  "type": "number"
                },
                "index_cost_cpu": {
                  "type": "number"
                },
                "row_cost_io": {
                  "type": "number"
                },
                "row_cost_cpu": {
                  "type": "number"
                },
                "max_index_blocks": {
                  "type": "number"
                },
                "max_row_blocks": {
                  "type": "number"
                },
                "copy_cost": {
                  "type": "number"
                }
              },
              "required": [
                "key_number",
                "num_records",
                "eq_ref",
                "index_cost_io",
                "index_cost_cpu",
                "row_cost_io",
                "row_cost_cpu",
                "max_index_blocks",
                "max_row_blocks",
                "copy_cost"
              ]
            }
          }
        },
        "required": [
          "name",
          "ddl",
          "num_of_records",
          "read_cost_io",
          "read_cost_cpu"
        ]
      }
    }
  },
  "required": [
    "current_database",
    "list_contexts"
  ]
};

evalp set @opt_context_schema='$opt_context_schema';
