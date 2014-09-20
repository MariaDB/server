// -*- js2-basic-offset: 2; indent-tabs-mode: nil -*-

"use strict";

function prim2html(prim, limit) {
  switch(typeof prim) {
  case 'undefined':
    return 'undefined';
  case 'boolean':
    return prim ? 'true' : 'false';
  case 'number':
    return String(prim);
  case 'string':
    if (prim.length > limit) {
      prim = prim.substring(0, limit) + '...';
    }
    return escapeHTML(prim);
  case 'array':
  case 'object':
    if (prim == null) {
      return 'null';
    } else if ($.isArray(prim)) {
      return 'array'; /* TODO: implement */
    } else {
      return 'object'; /* TODO: implement */
    }
  default:
    return 'ERROR';
  }
}

function escapeHTML(str) {
  return str.replace(/&/g, "&amp;")
            .replace(/"/g, "&quot;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;");
}
var Groonga = {
  key_type_list: ['Int8', 'UInt8', 'Int16', 'UInt16', 'Int32', 'UInt32',
                  'Int64', 'UInt64', 'Float', 'Time', 'ShortText',
                  'TokyoGeoPoint', 'WGS84GeoPoint'],
  value_type_list: ['Object', 'Bool',
                    'Int8', 'UInt8', 'Int16', 'UInt16', 'Int32', 'UInt32',
                    'Int64', 'UInt64', 'Float', 'Time'],
  column_type_list: ['Object', 'Bool',
                     'Int8', 'UInt8', 'Int16', 'UInt16', 'Int32', 'UInt32',
                     'Int64', 'UInt64', 'Float', 'Time', 'ShortText',
                     'Text', 'LongText', 'TokyoGeoPoint', 'WGS84GeoPoint'],
  tokenizer_list: ['TokenDelimit', 'TokenUnigram', 'TokenBigram', 'TokenTrigram', 'TokenMecab'],
  GRN_OBJ_PERSISTENT:             (0x01<<15),

  GRN_OBJ_TABLE_TYPE_MASK:        (0x07),
  GRN_OBJ_TABLE_HASH_KEY:         (0x00),
  GRN_OBJ_TABLE_PAT_KEY:          (0x01),
  GRN_OBJ_TABLE_NO_KEY:           (0x03),

  GRN_OBJ_KEY_WITH_SIS:           (0x01<<6),
  GRN_OBJ_KEY_NORMALIZE:          (0x01<<7),

  GRN_OBJ_COLUMN_TYPE_MASK:       (0x07),
  GRN_OBJ_COLUMN_SCALAR:          (0x00),
  GRN_OBJ_COLUMN_VECTOR:          (0x01),
  GRN_OBJ_COLUMN_INDEX:           (0x02),

  GRN_OBJ_COMPRESS_MASK:          (0x07<<4),
  GRN_OBJ_COMPRESS_NONE:          (0x00<<4),
  GRN_OBJ_COMPRESS_ZLIB:          (0x01<<4),
  GRN_OBJ_COMPRESS_LZO:           (0x02<<4),

  GRN_OBJ_WITH_SECTION:           (0x01<<7),
  GRN_OBJ_WITH_WEIGHT:            (0x01<<8),
  GRN_OBJ_WITH_POSITION:          (0x01<<9)
};

$.widget("ui.paginate", {
  version: "1.0",
  options: {
    total: 0,
    nItemsPerPage: 10,
    currentPage: 0,
    nShowLinks: 10,
    callback: null
  },
  _create: function() {
    var that = this;
    var element = this.element;
    element.addClass("pager");

    var total = this.options.total;
    var nItemsPerPage = this.options.nItemsPerPage;
    var currentPage = this.options.currentPage;
    var nShowLinks = this.options.nShowLinks;
    var lastPage = Math.floor((total - 1) / nItemsPerPage) + 1;
    var start = currentPage - Math.floor(nShowLinks / 2);
    start = (start < 1) ? 1 : start;
    var end = start + nShowLinks - 1;
    end = (end > lastPage) ? lastPage : end;

    var callback = this.options.callback;
    if (start > 1) {
      element.append($('<span />')
                     .addClass('pager')
                     .append($('<a />')
                             .attr('href', '#')
                             .text('1')
                             .click(function () {callback(0)})));
      element.append($('<span />').text('....'));
    }
    for (var i = start; i <= end; i++) {
      var page = $('<span />').append($('<a />')
                                      .attr('href', '#')
                                      .text(String(i))
                                      .click(function () {
                                        callback(Number($(this).text()) - 1);
                                      }));
      if (i == currentPage) {
        page.addClass('pager-current');
      } else {
        page.addClass('pager');
      }
      element.append(page);
    }
    if (end < lastPage) {
      element.append($('<span />')
                     .text('....'));
      element.append($('<span />')
                     .addClass('pager')
                     .append($('<a />')
                             .attr('href', '#')
                             .text(String(lastPage))
                             .click(function () {callback(lastPage - 1);})));
    }
  }
});

function GroongaAdmin() {
  this.current_table = null;
  this.statusTimer = null;
  this.semaphore = new Array();
  this.current_status = 0;
  this.reload_record_func = function(){};

  var that = this;
  this._initializeTabs();

  $('#tab-tablelist-link').click(function() {
    that.tablelist();
  });
  $('#tab-columnlist-link').click(function() {
    that.columnlist(that.current_table);
  });
  $('#tab-createrecord-link').click(function() {
    that.update_createrecord(that.current_table);
  });
  $('#tab-recordlist-link').click(function() {
    that.reload_record_func();
  });
  $('#createtable-add-table').click(function() {
    that.createtable();
  });
  $('#createrecord-add-record').click(function() {
    that.createrecord();
  });
  $('#createcolumn-add-column').click(function() {
    that.createcolumn();
  });
  $('#recordlist-remove-record').click(function() {
    that.removerecord();
  });
  $('#columnlist-remove-column').click(function() {
    that.removecolumn();
  });
  $('#tablelist-remove-table').click(function() {
    that.removetable();
  });
  $('#tab-recordlist-form').submit(function() {
    if ($('#table-tab-recordlist-full-checkbox').attr('checked')) {
      // full
      var d = {
        'table': that.current_table
      }
      $.each(that.SELECT_PARAMS, function(i, val) {
        var e = $('#tab-recordlist-' + val);
        if (e.val()) {
          d[val] = e.val();
        }
      });
      that.recordlist(d, true);
    } else {
      // simple
      that.recordlist_simple(
        that.current_table,
        $('#tab-recordlist-simplequery').val(),
        $('#tab-recordlist-simplequerytype').val(),
        1);
    }
    return false;
  });
  this._initializeSideMenu();
  this.update_tablelist();

  var e1 = $('#createtable-key-type-builtin');
  $.each(Groonga.key_type_list, function(i, val) {
    e1.append($('<option />').val(val).text(val));
  });

  e1 = $('#createtable-value-type-builtin');
  e1.append($('<option />').val('').text('なし'));
  $.each(Groonga.value_type_list, function(i, val) {
    e1.append($('<option />').val(val).text(val));
  });

  e1 = $('#createtable-default-tokenizer-builtin');
  e1.append($('<option />').val('').text('なし'));
  $.each(Groonga.tokenizer_list, function(i, val) {
    e1.append($('<option />').val(val).text(val));
  });

  e1 = $('#createcolumn-type-builtin');
  $.each(Groonga.column_type_list, function(i, val) {
    e1.append($('<option />').val(val).text(val));
  });

  $('#tab-recordlist-simplequerytype').change(function() {
    if ($(this).val() == 'scorer') {
      $('#tab-recordlist-incremental').hide();
      $('#tab-recordlist-incremental-label').hide();
    } else {
      $('#tab-recordlist-incremental').show();
      $('#tab-recordlist-incremental-label').show();
    }
    $('#tab-recordlist-incremental').change();

    var selectedOption = $(this).find(':selected');
    $('#tab-recordlist-simplequery').attr(
      'placeholder', selectedOption.data('placeholder')
    );
  }).change();

  $('#table-tab-recordlist-full-checkbox').change(function() {
    if ($(this).attr('checked')) {
      $('#table-tab-recordlist-form-simple').hide();
      $('#table-tab-recordlist-form-full').show();
    } else {
      $('#table-tab-recordlist-form-simple').show();
      $('#table-tab-recordlist-form-full').hide();
    }
  }).change();

  $('#tab-recordlist-incremental').change(function() {
    $('#tab-recordlist-simplequery').unbind('keyup');
    if ($(this).attr('checked') &&
        $('#tab-recordlist-simplequerytype').val() != 'scorer') {
      $('#tab-recordlist-simplequery').keyup(function(e) {
        that.recordlist_simple(
          that.current_table,
          $('#tab-recordlist-simplequery').val(),
          $('#tab-recordlist-simplequerytype').val(),
          1,
          true);
      });
    }
  }).change();

  $('#createcolumn-type').change(function(e) {
    var s = $('#createcolumn-type-table option:selected');
    var cs = $('#createcolumn-source');
    if (s.length > 0) {
      cs.empty().removeAttr('disabled');
      that.showloading(
        $.ajax({
          url: '/d/column_list',
          data: {'table': s.val()},
          dataType: 'json',
          success: function(d) {
            if(that.validateajax(d) < 0) { return; }
            var idx;
            var b = d[1];
            $.each(b[0], function(i, val) {
              if (val[0] == 'name') { idx = i; }
            });
            if (idx) {
              b.shift();
              $.each(b, function(i, val) {
                cs.append($('<option />').val(val[idx]).text(val[idx]));
              });
            }
            that.hideloading();
          },
          error: function(XMLHttpRequest, textStatus, errorThrown) {
            that.errorloading(XMLHttpRequest);
          }
        })
      );
    } else {
      cs.empty().attr('disabled', 'disabled');
    }
  });

  this.recordlist_count = 30;
};

jQuery.extend(GroongaAdmin.prototype, {
  SELECT_PARAMS: [
    'match_columns', 'query', 'filter',
    'scorer',
    'output_columns',
    'sortby', 'offset', 'limit',
    'drilldown',
    'drilldown_output_columns',
    'drilldown_sortby', 'drilldown_offset', 'drilldown_limit'
  ],
  _initializeTabs: function() {
    this._initializeDatabaseTab();
    this._initializeTableTab();
    this._initializeSuggestTab();
    this._selectTab("database");
  },
  _initializeDatabaseTab: function() {
    var that = this;

    this._$databaseTabs = $('#database-tabs').tabs({
      show: function(e, ui) {
	that.stop_status_timer();
	if (ui.panel.id == 'database-tab-summary') {
          that.start_status_timer();
	}
      }
    });
  },
  _initializeTableTab: function() {
    this._$tableTabs = $('#table-tabs').tabs({
      show: function(e, ui) {
      }
    });
  },
  _initializeSuggestTab: function() {
    var that = this;

    this._$suggestTabs = $('#suggest-tabs').tabs({
      show: function(e, ui) {
      }
    });

    this._initializeSuggestDatasetComplete();
    this._initializeSuggestQueryComplete();
    this._initializeSuggestSubmit();
    this._initializeSuggestResult();
  },
  _initializeSuggestDatasetComplete: function() {
    var that = this;
    var $dataset = $("#suggest-dataset");
    this._$suggestDataset = $dataset;
    $dataset.autocomplete({
      minLength: 0,
      source: function (request, response) {
	var datasets = [];
	$.each(that._tables, function(i, table_name) {
          var suggestTableMatch = /^item_(.+)$/.exec(table_name);
          if (suggestTableMatch) {
            var dataset = suggestTableMatch[1];
            datasets.push(dataset);
          }
        });

        datasets = $.ui.autocomplete.filter(datasets, request.term);
        response(datasets);
      }
    });
    $dataset.focus(function (event) {
      $dataset.autocomplete("search", $dataset.val());
    });
  },
  _suggestParameters: function(query, dataset, type) {
    var nItemsPerPage = 30;
    return {
      query: query,
      types: type,
      table: "item_" + dataset,
      column: "kana",
      offset: 0,
      limit: nItemsPerPage,
    };
  },
  _initializeSuggestQueryComplete: function() {
    var that = this;
    this._$suggestQuery = $("#suggest-query").autocomplete({
      source: function (request, response) {
        var $dataset = $("#suggest-dataset");
        var dataset = $dataset.val();
        $("#suggest-submit").click();
        $.ajax({
          url: "/d/suggest",
          data: that._suggestParameters(request.term, dataset, "complete"),
          dataType: "jsonp",
          success: function (data, textStatus, jqXHR) {
            var completions = data[1]["complete"];
            var items = [];
            if (completions && completions.length > 2) {
              completions.shift();
              completions.shift();
              $.each(completions, function(i, item) {
                var key = item[0];
                items.push(key);
                if (items.length >= 3) {
                  return false;
                }
                return true;
              });
            }
            response(items);
          },
          error: function(jqXHR, textStatus, errorThrown) {
          }
	});
      }
    });
  },
  _initializeSuggestSubmit: function() {
    var that = this;
    $("#suggest-submit").click(function (event) {
      var dataset = $("#suggest-dataset").val();
      var query = $("#suggest-query").val();
      var type = that._suggestResultType;
      var parameters = that._suggestParameters(query, dataset, type);
      $.ajax({
        url: "/d/suggest",
        data: parameters,
        dataType: "jsonp",
        success: function (data, textStatus, jqXHR) {
          var response = data[1][type];
          response.shift();
          var $result = $("#suggest-result-tab-" + type);
          $result
            .empty()
            .append($("<div/>").append(that._createResultTable(response)));
        },
        error: function(jqXHR, textStatus, errorThrown) {
        }
      });
    });
  },
  _initializeSuggestResult: function() {
    var that = this;
    $("#suggest-result-tabs").tabs({
      show: function (event, ui) {
        that._suggestResultType = ui.panel.id.replace(/^suggest-result-tab-/, "");
        $("#suggest-submit").click();
      }
    });
  },
  _selectTab: function(name) {
    this.stop_status_timer();
    this._$databaseTabs.hide();
    this._$tableTabs.hide();
    this._$suggestTabs.hide();
    switch (name) {
    case "table":
      this._$tableTabs.show();
      break;
    case "suggest":
      this._$suggestTabs.show();
      break;
    case "database":
    default:
      this._$databaseTabs.show();
      break;
    }
  },
  _initializeSideMenu: function () {
    var that = this;
    $('#side-menu-summary').click(function() {
      that.current_table = null;
      that._selectTab("database");
      that._$databaseTabs.tabs("select", "#database-tab-summary");
    });
    $('#side-menu-suggest').click(function() {
      that.current_table = null;
      that._selectTab("suggest");
    });
  },
  start_status_timer: function() {
    var that = this;
    this.stop_status_timer();
    this.status();
    this.statusTimer = setInterval(function() {that.status()}, 1000);
  },
  change_status_timer: function(time) {
    var that = this;
    this.stop_status_timer();
    this.statusTimer = setInterval(function() {that.status()}, time);
  },
  stop_status_timer: function() {
    if (this.statusTimer) {
      clearInterval(this.statusTimer);
      this.statusTimer = null;
    }
  },
  _createResultTable: function (result, options) {
    var that = this;
    if (!options) {
      options = {};
    }
    var table = $('<table class="records"/>');
    if ($.isArray(result)) {
      var nEntries = result.length;
      if (nEntries >= 1) {
        var thead = $('<thead/>');
        table.append(thead);
        var line = result[0];
        if ($.isArray(line)) {
          var tr = $('<tr/>');
          thead.append(tr);
          var m = line.length;
          if (options.check) {
            tr.append($('<th/>'));
          }
          for (var j = 0; j < m; j++) {
            var th = $('<th/>');
            tr.append(th);
            th.append(prim2html(line[j][0], 128));
            th.append($('<br />'));
            th.append(prim2html(line[j][1], 128));
          }
          if (options.button) {
            tr.append($('<th/>'));
          }
        }
        var tbody = $('<tbody>');
        table.append(tbody);
        for (var i = 1; i < nEntries; i++) {
          line = result[i];
          if ($.isArray(line)) {
            var tr = $('<tr>');
            table.append(tr);
            var m = line.length;
            switch(options.check) {// チェックボックスの値を何にするか
            case 1: // 1番目の要素(レコード一覧の_id等)
            case 2: // 2番目の要素(テーブル・カラム一覧のname等)
              var td = $('<td/>');
              tr.append(td);
              td.append($('<input/>')
                        .attr("type", "checkbox")
                        .attr("value", line[options.check-1]));
              break;
            }
            for (var j = 0; j < m; j++) {
              var td = $('<td/>');
              tr.append(td);
              td.append(prim2html(line[j], 128));
            }
            switch(options.button) {
            case 1: // Edit record
              var td = $('<td/>');
              tr.append(td);
              td.append($('<input/>')
                        .attr("type", "button")
                        .attr("value", "編集")
                        .attr("data-record-id", line[0])
                        .click(function () {
                          that.show_edit_record($(this).attr("data-record-id"));
                        }));
              break;
            case 2: // Table
              var td = $('<td/>');
              tr.append(td);
              td.append($('<input/>')
                        .attr("type", "button")
                        .attr("value", "詳細")
                        .attr("data-table-name", line[1])
                        .click(function () {
                          var tableName = $(this).attr("data-table-name");
                          $("#side-menu-tablelist-link-" + tableName).click();
                        }));
              break;
            }
          }
        }
      }
    }
    return table;
  },
  show_edit_record: function(id) {
    $('#table-tabs').tabs('select', 2);
    this.update_createrecord(this.current_table, id);
  },
  format_unix_time: function(unix_time) {
    var date = new Date();
    date.setTime(unix_time * 1000);
    return date.toLocaleString();
  },
  format_duration: function(duration_in_seconds) {
    var duration = "";
    var days = Math.floor(duration_in_seconds / 3600 / 24);
    var hours = Math.floor(duration_in_seconds / 3600 % 24);
    var minutes = Math.floor(duration_in_seconds / 60 % 60);
    var seconds = Math.floor(duration_in_seconds % 60);

    if (days > 0) {
      duration += days;
      if (days == 1) {
        duration += " day, ";
      } else {
        duration += " days, ";
      }
    }
    if (days > 0 || hours > 0) {
      duration += hours + ":" + minutes + ":" + seconds;
    } else if (minutes > 0) {
      duration += minutes + ":" + seconds;
    } else {
      duration += seconds;
    }

    return duration;
  },
  maxThroughput: 0,
  lastNQueries: -1,
  keepLastNData: 100,
  throughputData: [],
  throughputChart: null,
  updateThroughputChart: function(statusData) {
    var maxThroughputUpdated = false;
    if (this.lastNQueries >= 0) {
      var throughput = statusData.n_queries - this.lastNQueries;
      this.throughputData.push(throughput);
      if (this.maxThroughput < throughput) {
        this.maxThroughput = throughput;
        maxThroughputUpdated = true;
      }
    }
    if (this.throughputData.length > this.keepLastNData) {
       this.throughputData.shift();
    }
    if (!this.throughputChart) {
      this.throughputChart = $.plot($("#throughput-chart"),
                                    [[]],
                                    {xaxis: {min: -(this.keepLastNData - 1),
                                             max: 0},
                                     yaxis: {min: 0}});
    }
    var that = this;
    var chartSeries = $.map(this.throughputData, function(n, i) {
                         return [[-(that.throughputData.length - i) + 1, n]];
                      });
    this.throughputChart.setData([chartSeries]);
    if (maxThroughputUpdated) {
      this.throughputChart.setupGrid();
    }
    this.throughputChart.draw();
    this.lastNQueries = statusData.n_queries;
  },
  status: function() {
    if (this.current_status > 0) { return; }
    this.current_status++;
    var that = this;
    $.ajax({
      url: '/d/status',
      data: {},
      dataType: 'json',
      success: function(b) {
        that.current_status--;
        if (!b) {
          that.change_status_timer(10000);
          return;
        }
        var d = b[1];
        $('#status-starttime').text(that.format_unix_time(d.starttime));
        $('#status-uptime').text(that.format_duration(d.uptime));
        $('#status-n-queries').text(d.n_queries);
        $('#status-cache-hit-rate').text(d.cache_hit_rate);
        that.updateThroughputChart(d);
        that.change_status_timer(1000);
      },
      error: function() {
        that.current_status--;
        that.change_status_timer(10000);
      }
    });
  },
  update_tablelist: function() {
    var that = this;
    this._tables = [];
    this.showloading(
      $.ajax({
        url: '/d/table_list',
        data: {},
        dataType: 'json',
        success: function(d) {
          if (that.validateajax(d) < 0) { return; }
          d.shift();
          var tl = $('#side-menu-tablelist').empty();
          var tt = $('#createtable-key-type-table').empty();
          var vt = $('#createtable-value-type-table').empty();
          var ct = $('#createcolumn-type-table').empty();
          var b = d.shift();
          b.shift();
          $.each(b, function(i, val) {
            var table_name = val[1];
	    that._tables.push(table_name);
            tl.append(
              $('<li />').append(
                $('<a />')
                  .attr('id', 'side-menu-tablelist-link-' + table_name)
                  .attr('href', '#side-menu-tablelist-' + table_name)
                  .text(table_name)
                  .click(function() {
                    that.current_table = table_name;
                    $('#database-tabs').hide();
                    $('#suggest-tabs').hide();
                    that.stop_status_timer();
                    $('#table-tabs').show();
                    that.columnlist(table_name);
                    $('#tab-recordlist-simplequery').val('');
                    that.recordlist_simple(table_name, null, null, 1);
                    that.update_createrecord(that.current_table);
                  })
              )
            );
            tt.append($('<option />').val(val[1]).text(val[1]));
            vt.append($('<option />').val(val[1]).text(val[1]));
            ct.append($('<option />').val(val[1]).text(val[1]));
          });
          that.hideloading();
        },
        error: function(XMLHttpRequest, textStatus, errorThrown) {
          that.errorloading(XMLHttpRequest);
        }
      })
    );
  },
  tablelist: function() {
    $('#tab-tablelist-table').empty();
    var that = this;
    this.showloading(
      $.ajax({
        url: '/d/table_list',
        data: {},
        dataType: 'json',
        success: function(d) {
          if (that.validateajax(d) < 0) { return; }
          var b = d[1];
          var table = that._createResultTable(b, {check: 2, button: 2});
          $('#tab-tablelist-table').append($('<h1 />').text('テーブル一覧')).append(table);
          that.hideloading();
        },
        error: function(XMLHttpRequest, textStatus, errorThrown) {
          that.errorloading(XMLHttpRequest);
        }
      })
    );
  },
  recordlist_simple: function(table_name, simplequery, simplequery_type, page, hide_dialog) {
    var d = {
      'table': table_name,
      'offset': (page - 1) * this.recordlist_count,
      'limit': this.recordlist_count
    }
    switch (simplequery_type) {
    case 'query':
    case 'filter':
    case null:
      if (simplequery) {
        d[simplequery_type] = simplequery;
      }
      this.recordlist(d, true, hide_dialog);
      break;
    }
  },
  recordlist: function(params, show_pager, hide_dialog) {
    var that = this;
    this.reload_record_func = function(){
      that.recordlist(params, show_pager, hide_dialog);
    };
    this.showloading(
      $.ajax({
        url: '/d/select',
        data: params,
        dataType: 'json',
        success: function(d) {
          if (that.validateajax(d, hide_dialog) < 0) { return; }
          var rc = d.shift();
          if (rc[0] != 0) {
            alert('error: ' + rc[3]);
            that.hideloading();
            return false;
          }
          var body = d.shift();
          var recs = body.shift();
          var all_count = recs.shift()[0];
          var pager;
          if (show_pager) {
            var offset = params['offset'] || 0;
            var rows = params['limit'] || 10;
            if (rows < 0){
              rows = all_count + parseInt(rows) + 1;
            }
            if (rows != '' && !parseInt(rows)) {
              pager = $('<span />');
            } else {
              pager = $("<div/>");
              pager.paginate({
                total: all_count,
                nItemsPerPage: rows,
                currentPage: Math.floor(offset/rows)+1,
                callback: function(page) {
                  params['offset'] = page * rows;
                  that.recordlist(params, true, false);
                  return false;
                }
              });
            }
          } else {
            pager = $('<span />');
          }
          $('#tab-recordlist-table')
            .empty()
            .append($('<h1 />').text('レコード一覧: ' + params['table']))
            .append($('<p />').text('総件数: ' + all_count))
            .append(pager.clone(true))
            .append($('<div />').append(that._createResultTable(recs, {check: 1, button: 1})))
            .append(pager);
          that.hideloading();
        },
        error: function(XMLHttpRequest, textStatus, errorThrown) {
          that.errorloading(XMLHttpRequest, hide_dialog);
        }
      })
    ,hide_dialog);
  },
  columnlist: function(table_name) {
    var that = this;
    $('#tab-columnlist-table').empty();
    this.showloading(
      $.ajax({
        url: '/d/column_list',
        data: {'table': table_name},
        dataType: 'json',
        success: function(d) {
          if (that.validateajax(d) < 0) { return; }
          var b = d[1];
          var table = that._createResultTable(b, {check: 2});
          $('#tab-columnlist-table')
            .append($('<h1 />').text('カラム一覧: ' + table_name))
            .append(table);
          that.hideloading();
        },
        error: function(XMLHttpRequest, textStatus, errorThrown) {
          that.errorloading(XMLHttpRequest);
        }
      })
    );
  },
  add_record_inputbox: function(type, value) {
    var inputbox = null;
    switch(type){
    case "Bool":
      inputbox = $('<input />')
          .attr("type","checkbox")
          .attr("value","true");
      if (value) {
        inputbox.attr("checked","");
      }
      break;
    case "UInt8":
    case "UInt16":
    case "UInt32":
    case "UInt64":
    case "Int8":
    case "Int16":
    case "Int32":
    case "Int64":
    case "Float":
      inputbox = $('<input />')
          .attr("type", "text")
          .val(isNaN(value) ? "" : value);
      break;
    case "Text":
    case "ShortText":
    case "LongText":
      inputbox = $('<textarea />')
          .attr("cols", "50")
          .attr("rows", "2")
          .text(value ? value : "");
      break;
    case "TokyoGeoPoint":
    case "WGS84GeoPoint":
    case "Time":
      inputbox = $('<input />')
          .attr("type", "text")
          .attr("size", "40")
          .val(value ? value : "");
      break;
    case "Object":
      inputbox = $('<input />')
          .attr("type", "text")
          .attr("disabled", "disabled");
      break;
    default:
      inputbox = $('<input />')
          .attr("type", "text")
          .val(value ? value : "");
    }
    inputbox.addClass('column_values');
    return inputbox;
  },
  add_record_deletebutton: function(){
    var ret =
      $('<span />')
        .append("[×]")
        .css('cursor', 'pointer')
        .click(function() {
          $(this).prev().remove();
          $(this).next().remove();
          $(this).remove();
        });
    return ret;
  },
  update_createrecord_loadcomplete: function(d_sel, d_col) {
    var that = this;
    var b = d_sel[1][0];
    var columns = $('<tbody />');
    var listofs = b[1].length - (d_col[1].length - 1);
    for (var i = 1; i < b[1].length; i++) {
      var line = b[1][i];
      var value = null;
      if (b[2]) value = b[2][i];
      if ($.isArray(line)) {
        var tr = $('<tr/ >')
          .addClass('create-record-columns')
          .append(
            $('<td />')
              .addClass('columnname')
              .append(prim2html(line[0], 128))
          )
          .append(
            $('<td />')
              .addClass('columntype')
              .append("(")
              .append($('<span />')
                .append(prim2html(line[1], 128))
              )
              .append(")")
          );
        var inputtd = $('<td />').addClass('columnval');
        if (i >= listofs && d_col[1][i - listofs + 1][4].indexOf("COLUMN_VECTOR") >= 0){
          var type = line[1];
          if (value != null) {
            for (var j = 0; j < value.length; j++) {
              inputtd
                .append(this.add_record_inputbox(line[1], value[j]))
                .append(this.add_record_deletebutton())
                .append('<br />');
            }
          }
          inputtd
            .append($('<span />')
              .append("[値を追加]")
              .css('cursor', 'pointer')
              .click(function() {
                var target = $(this).parent();
                target
                  .append(that.add_record_inputbox($(this).parent().prev().children().text()))
                  .append(that.add_record_deletebutton())
                  .append("<br />");
                $(this).appendTo(target);
              })
            );
        } else {
          inputtd.append(this.add_record_inputbox(line[1], value));
          if (line[0] == "_key" && value != null) {
            inputtd.children().attr("disabled", "disabled");
          }
        }
        tr.append(inputtd);
        columns.append(tr);
      }
    }
    $("#table-createrecord").append(columns);
    this.hideloading();
  },
  update_createrecord: function(table_name, id) {
    var that = this;
    var d_sel = null;
    var d_col = null;
    $('#table-createrecord').empty();
    this.showloading(
      $.ajax({
        url: '/d/select',
        data: {
          'table' : table_name,
          'limit' : 1,
          'query' : '_id:' + id
        },
        dataType: 'json',
        success: function(d) {
          if (that.validateajax(d) < 0) { return; }
          d_sel = d;
          if (d_col) {
            that.update_createrecord_loadcomplete(d_sel, d_col);
          }
        },
        error: function(XMLHttpRequest, textStatus, errorThrown) {
          that.errorloading(XMLHttpRequest);
        }
      })
    );
    this.showloading(
      $.ajax({
        url: '/d/column_list',
        data: {
          'table' : table_name
        },
        dataType: 'json',
        success: function(d) {
          if (that.validateajax(d) < 0) { return; }
          d_col = d;
          if (d_sel) {
            that.update_createrecord_loadcomplete(d_sel, d_col);
          }
        },
        error: function(XMLHttpRequest, textStatus, errorThrown) {
          that.errorloading(XMLHttpRequest);
        }
      })
    );
  },
  createtable: function() {
    var that = this;
    var flags = 0;
    $('#createtable-flags>input:checked').each(function() {
      flags |= Groonga[$(this).val()];
    });
    flags |= Groonga[$('#createtable-key-index').val()];
    this.showloading(
      $.ajax({
        url: '/d/table_create',
        data: {
          name: $('#createtable-name').val(),
          'flags': flags,
          key_type: $('#createtable-key-type').val(),
          value_type: $('#createtable-value-type').val(),
          default_tokenizer: $('#createtable-default-tokenizer').val()
        },
        dataType: 'json',
        success: function(d) {
          if (that.validateajax(d) < 0) { return; }
          that.hideloading();
          alert('テーブルを作成しました。');
          that.update_tablelist();
        },
        error: function(XMLHttpRequest, textStatus, errorThrown) {
          that.errorloading(XMLHttpRequest);
        }
      })
    );
  },
  createcolumn: function() {
    var that = this;
    var flags = 0;
    $('#createcolumn-flags>input:checked').each(function() {
      flags |= Groonga[$(this).val()];
    });
    $('#createcolumn-ii-flags>input:checked').each(function() {
      flags |= Groonga[$(this).val()];
    });
    flags |= Groonga[$('#createcolumn-column-type').val()];
    flags |= Groonga[$('#createcolumn-column-compress').val()];
    var d = {
      table: this.current_table,
      name: $('#createcolumn-name').val(),
      'flags': flags,
      type: $('#createcolumn-type').val()
    };
    if ($('#createcolumn-source').val()) {
      d['source'] = $('#createcolumn-source').val();
    }
    this.showloading(
      $.ajax({
        url: '/d/column_create',
        data: d,
        dataType: 'json',
        success: function(d) {
          if (that.validateajax(d) < 0) { return; }
          that.hideloading();
          alert('カラムを作成しました。');
        },
        error: function(XMLHttpRequest, textStatus, errorThrown) {
          that.errorloading(XMLHttpRequest);
        }
      })
    );
  },
  createrecord_getvalue: function(type, inputbox) {
    switch(type){
    case "Bool":
      if (inputbox.is('input:checked')) {
        return true;
      } else {
        return false;
      }
    default:
      return inputbox.val();
    }
  },
  createrecord: function() {
    var that = this;
    var d = {};
    $('.create-record-columns').each(function() {
      if (!$(this).children('.columnval').children().attr('disabled')
        || $(this).children('.columnname').text() == "_key") {
        var type = $(this).children('.columntype').children().text();
        if ($(this).children('.columnval').children('span').length) {
          var arr = [];
          $(this).children('.columnval').children('.column_values').each(function() {
            arr.push(that.createrecord_getvalue(type, $(this)));
          });
          d[$(this).children('.columnname').text()] = arr;
        } else {
          d[$(this).children('.columnname').text()] =
            that.createrecord_getvalue(type, $(this).children('.columnval').children());
        }
      }
    });
    this.showloading(
      $.ajax({
        url: '/d/load',
        data: {
          "table" : this.current_table,
          "input_type" : "json",
          "output_type" : "json",
          "values" : JSON.stringify([d])
        },
        dataType: 'json',
        success: function(d) {
          if (that.validateajax(d) < 0) { return; }
          that.hideloading();
          alert('レコードを作成しました。');
        },
        error: function(XMLHttpRequest, textStatus, errorThrown) {
          that.errorloading(XMLHttpRequest);
        }
      })
    );
  },
  removerecord: function() {
    var that = this;
    var checklist = $("#tab-recordlist-table").find("input:checked");
    var completecount = checklist.length;
    if (completecount > 0) {
      $('<div />')
        .append("選択した" + completecount + "件のレコードを削除しますか？")
        .dialog({
          modal: true,
          buttons: {
            'いいえ': function() {
              $(this).dialog('close');
            },
            'はい': function() {
              $(this).dialog('close');
              checklist.each(function(i, val) {
                that.showloading(
                  $.ajax({
                    url: '/d/delete',
                    data: {
                      "table" : that.current_table,
                      "id" : val.value
                    },
                    dataType: 'json',
                    success: function() {
                      if (--completecount == 0) {
                        $('#tab-recordlist-form').submit();
                        alert('レコードを削除しました。');
                      } else if (completecount < 0){
                        that.hideloading();
                      }
                    },
                    error: function(XMLHttpRequest, textStatus, errorThrown) {
                      completecount = 0;
                      that.errorloading(XMLHttpRequest);
                    }
                  })
                );
              });
            }
          }
        });
    }
  },
  removecolumn: function() {
    var that = this;
    var checklist = $("#tab-columnlist-table").find("input:checked");
    var completecount = checklist.length;
    if (completecount) {
      $('<div />')
        .append("選択した" + completecount + "件のカラムを削除しますか？")
        .dialog({
          modal: true,
          buttons: {
            'いいえ': function() {
              $(this).dialog('close');
            },
            'はい': function() {
              $(this).dialog('close');
              checklist.each(function(i, val) {
                that.showloading(
                  $.ajax({
                    url: '/d/column_remove',
                    data: {
                      "table" : that.current_table,
                      "name" : val.value
                    },
                    dataType: 'json',
                    success: function() {
                      if (!(--completecount)) {
                        that.columnlist(that.current_table);
                        alert('カラムを削除しました。');
                      } else if (completecount < 0){
                        that.hideloading();
                      }
                    },
                    error: function(XMLHttpRequest, textStatus, errorThrown) {
                      completecount = 0;
                      that.errorloading(XMLHttpRequest);
                    }
                  })
                );
              });
            }
          }
        });
    }
  },
  removetable: function() {
    var that = this;
    var checklist = $("#tab-tablelist-table").find("input:checked");
    var completecount = checklist.length;
    if (completecount > 0) {
      $('<div />')
        .append("選択した" + completecount + "件のテーブルを削除しますか？")
        .dialog({
          modal: true,
          buttons: {
            'いいえ': function() {
              $(this).dialog('close');
            },
            'はい': function() {
              $(this).dialog('close');
              checklist.each(function(i, val) {
                that.showloading(
                  $.ajax({
                    url: '/d/table_remove',
                    data: {
                      "name" : val.value
                    },
                    dataType: 'json',
                    success: function() {
                      if (--completecount == 0) {
                        that.tablelist();
                        that.update_tablelist();
                        alert('テーブルを削除しました。');
                      } else if (completecount < 0){
                        that.hideloading();
                      }
                    },
                    error: function(XMLHttpRequest, textStatus, errorThrown) {
                      completecount = 0;
                      that.errorloading(XMLHttpRequest);
                    }
                  })
                );
              });
            }
          }
        });
    }
  },
  showloading: function(obj, hide_dialog) {
    var that = this;
    if (obj == null) { return; }
    this.semaphore[this.semaphore.length] = obj;
    if ( $("#loadingdialog").size() > 0 || hide_dialog) { return; }
    $("<div />")
      .attr("id", "loadingdialog")
      .attr("style", "text-align: center;")
      .append($("<img />").attr("src", "images/loading.gif"))
      .append(" Loading...")
      .dialog({
        title: "",
        width: 200,
        height: 110,
        minHeight: 110,
        modal: true,
        resizable: false,
        draggable: false,
        position: ["right", "bottom"],
        autoOpen: false,
        buttons: {
          '中止': function() {
            if (obj) { obj.abort(); }
            that.hideloading();
          }
        }
      });
    $("#loadingdialog").parents(".ui-dialog").children(".ui-dialog-titlebar").remove();
    $("#loadingdialog").dialog("open");
    $(".ui-widget-overlay").css("opacity", "0.0");
  },
  hideloading: function() {
    for (var i = 0; i < this.semaphore.length; i++) {
      if ( this.semaphore[i].readyState == 4) {
        this.semaphore.splice(i, 1);
        i--;
      }
    }
    if ( this.semaphore.length == 0) {
      $("#loadingdialog").dialog("close");
      $("#loadingdialog").remove();
    }
  },
  errorloading: function(ajax, hide_dialog) {
    var that = this;
    var json = null;
    if (ajax) {
      json = JSON.parse(ajax.responseText);
    }
    this.hideloading();
    for (var i = 0; i < this.semaphore.length; i++) {
      this.semaphore[i].abort();
      this.semaphore.splice(i, 1);
      i--;
    }
    if ( $("#loadingdialog").size() == 0 && !hide_dialog) {
      var error_label;
      var error_message;
      if (json){
        error_label = "groongaでエラーが発生しました。";
        error_message = json[0][3] + "(" + json[0][0] + ")";
      } else if (ajax) {
        error_label = "通信エラーが発生しました。";
        error_message = "" + ajax.status + ": " + ajax.statusText;
      } else {
        error_label = "通信エラーが発生しました。";
        error_message = "";
      }
      $("<div />")
        .append($("<div />").text(error_label))
        .append($("<div />").text(error_message))
        .attr("id", "loadingdialog")
        .dialog({
          title: "",
          width: 340,
          height: 160,
          minHeight: 160,
          modal: true,
          resizable: false,
          draggable: false,
          open: function() {
            $(this).parents(".ui-dialog").children(".ui-dialog-titlebar").remove();
          },
          buttons: { OK: function() { that.hideloading(); } }
        });
    }
  },
  validateajax: function(d, hide_dialog) {
    if (!d) {
      this.errorloading(null, hide_dialog);
      return -1;
    }
    return 0;
  }
});
