#!/bin/sh

base_dir="$(dirname $0)"

fixture_dir="${srcdir}/fixtures/geo-select"
data_dir="${base_dir}/fixtures/geo-select"
csv_xz="${fixture_dir}/13_2010.CSV.xz"
csv="${data_dir}/13_2010.CSV"
grn="${data_dir}/load.grn"

geo_select_generate_grn_rb="${base_dir}/geo-select-generate-grn.rb"

db="${base_dir}/tmp/geo-select/db"

bench_geo_select="./bench-geo-select"

mkdir -p "${data_dir}"
if [ ! -s "${csv}" ] || [ "${csv}" -ot "${csv_xz}" ]; then
    echo "extracting ${csv_xz}..."
    xzcat "${csv_xz}" | iconv --from-code cp932 --to-code utf-8 > "${csv}"
fi

if [ ! -s "${grn}" ] || [ "${grn}" -ot "${csv}" ]; then
    echo "generating test data..."
    "${RUBY}" "${geo_select_generate_grn_rb}" "${csv}" "${grn}"
fi

if [ ! -s "${db}" ] || [ "${db}" -ot "${grn}" ]; then
    echo "creating test database..."
    rm -rf "$(dirname ${db})"
    mkdir -p "$(dirname ${db})"
    "${GROONGA}" -n "${db}" < "${grn}"
fi

if [ "${GROONGA_BENCH_DEBUG}" = "yes" ]; then
    bench_geo_select="../../libtool --mode=execute gdb --args ${bench_geo_select}"
fi
${bench_geo_select}
