import shutil, os, sys

def process_file(input_file, output_file, project):
    with open(input_file, 'r') as infile, open(output_file, 'w') as outfile:
        for line in infile:
            if "AdditionalDependencies" in line:
                modified_line = ""
                if project == 's':
                    modified_line = ';'.join([word for word in line.split(';') if "mysys.lib" not in word])
                    modified_line = ';'.join([word for word in modified_line.split(';') if "dbug.lib" not in word])
                    modified_line = ';'.join([word for word in modified_line.split(';') if "strings.lib" not in word])
                else:
                    modified_line = ';'.join([word for word in line.split(';') if "sql_builtins.lib" not in word])
                outfile.write(modified_line + '\n')
            else:
                outfile.write(line)

output_file = 'temp.txt'

def main(server, backup):
    process_file(server, output_file, 's')
    shutil.move(output_file, server)

    process_file(backup, output_file, 'b')
    shutil.move(output_file, backup)

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python remove_dup_lib.py <build_dir_name>")
        sys.exit(1)
    root = os.path.dirname(os.path.abspath(__file__)) + "\\..\\" + sys.argv[1] + "\\"
    main(root + "sql\\server.vcxproj", root + "extra\\mariabackup\\mariadb-backup.vcxproj")
