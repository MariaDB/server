import json
import re
from pathlib import Path

def create_json_and_html(engine, benchmarks, template_html):
    """Generates JSON and HTML file for a given engine."""

    # Create the JSON structure
    output_json = {
        "context": {
            "date": "2026-02-26T00:00:00+00:00",
            "host_name": "unknown",
            "executable": "unknown",
            "num_cpus": 0,
            "mhz_per_cpu": 0,
            "cpu_scaling_enabled": False,
            "aslr_enabled": False,
            "caches": [],
            "load_avg": [],
            "library_version": "unknown",
            "library_build_type": "release",
            "json_schema_version": 1
        },
        "benchmarks": benchmarks
    }

    # Generate JSON file
    json_file_path = f'benchmark/results_{engine.lower()}.json'
    with open(json_file_path, 'w') as f:
        json.dump(output_json, f, indent=2)
    print(f"Generated {json_file_path}")


    # Generate HTML file
    json_data_string = json.dumps(output_json)
    html_content = template_html.replace('{{JSON_DATA}}', json_data_string)
    html_content = html_content.replace('Memcpy Performance Comparison', f'Memcpy Performance Comparison ({engine})')
    html_content = html_content.replace('Size (Bytes)', 'reclength')


    html_file_path = f'{engine.lower()}_results.html'
    with open(html_file_path, 'w') as f:
        f.write(html_content)
    print(f"Generated {html_file_path}")


def main():
    """Main function to parse data and generate files."""
    p = Path('results_final.plain')
    raw_data = p.read_text()

    innodb_benchmarks = []
    myisam_benchmarks = []
    current_benchmark_name = None

    for line in raw_data.strip().split('\n'):
        if line.startswith('---'):
            current_benchmark_name = line.strip().replace('---', '').strip()
        elif ':' in line:
            match = re.match(r'(\w+)/(\d+):\s+([\d\.]+)', line)
            if match:
                engine, size_str, cpu_time_str = match.groups()
                size = int(size_str) + 5
                cpu_time = float(cpu_time_str) / 5

                benchmark_entry = {
                    "name": f"{current_benchmark_name}/{size}",
                    "cpu_time": cpu_time,
                    "time_unit": "ns/call"
                }

                if engine == 'InnoDB':
                    innodb_benchmarks.append(benchmark_entry)
                elif engine == 'MyISAM':
                    myisam_benchmarks.append(benchmark_entry)

    # Read the template html file
    with open('benchmark/index.template.html', 'r') as f:
        template_html = f.read()

    # Generate files for InnoDB
    create_json_and_html('InnoDB', innodb_benchmarks, template_html)

    # Generate files for MyISAM
    create_json_and_html('MyISAM', myisam_benchmarks, template_html)

if __name__ == '__main__':
    main()
