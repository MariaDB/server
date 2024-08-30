#!/usr/bin/python3
import pdb
import re
from dataclasses import dataclass
import bisect
import argparse
################################################################################
# How this script works
# The script is mainly driven by a state machine that consumes input
# and produces output "record-by-record"in an iterator-like fashion. Coroutines,
# are used to consume each of the inputs only when they are needed for each
# state, assuring proper rate-matching as 3 input sources are utilized to
# determine the insertion point of the new language, and not all
# 3 inputs are consumed at the same rate.
# The following steps are performed by the script to insert translations
# of the new language into a copy of the errmsg-utf8.txt file:
# 1. Load the source file and map out the lines in a data structure
# 2. Start reading the source file line by line.
#     2.1 For each line you can be in
#          2.1.1 SEARCHING_FOR_NEXT_HEADER state
#                - In this state, we continually search the incoming 
#                  lines from the source file for a string starting
#                  with a series of capital letters (^[A-Z]+).
#                - Write each line to the output file, which is a copy
#                  of 'errmsg-utf8.txt'.
#                - Change the state to CALCULATE_INSERT_POINT if a string matching
#                  the previous criteria is found
#                - Take the string starting with capitals and save it in
#                  the current_header variable"
#          2.1.2 CALCULATE_INSERT_POINT state
#                - Go to the data structure for the source file and
#                  using te current_header as a key, read out the
#                  value part of the structure. The value part should be
#                  a list .
#                - Find the insert point for the new language
#                  error message based on the list from the previous step.
#                - Change state to PERFORM_INSERT
#          2.1.3 PERFORM_INSERT state
#                - Read the source file and copy out each line to the output
#                  file (the copy of 'errmsg-utf8.txt').
#                - Continue reading the source file and checking if the
#                  insert point has been reached. Once it has been reached
#                  insert the new language in the output file.
#                - Change state to SEARCHING_FOR_NEXT_HEADER 
################################################################################

class SectionList(list):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.comment_locations = []


def read_file(filename):
    ''' A function that reads a file in one go '''
    with open(filename, 'r') as f:
        data = f.read()
    return data

def obtain_key_value_from_translation_line(translation_match, line):
    if translation_match :
        return translation_match.groups()
    else:
        return '#', line # Here we assume some other line type
    #that is not a language type and its translation. Return as is with hash character as key

def map_out_source_data(data):
    '''
    Load the source error message file into a navigable data structure (a lists of lists
    has been chosen to ensure the source order is not disrupted)
    '''
    # use a regex to split the data into sections
    sections = re.split(r'\n(?=[A-Z])', data)
    # create a dictionary to store the processed data
    data_dict = {}
    # process each section
    for section in sections:
        if not re.match(r'^[A-Z]+', section):
            continue
        # split the section into lines
        lines = section.split('\n')
        # the title of the section is the first line
        title = lines[0].strip()
        # create a list for the key-value pairs in this section
        section_list = []
        comment_list = []
        prev_key = ''
        current_line_loc = 0
        # process each line (except the first one)
        for line in lines[1:]:
            # split the line into a key and a value
            print(line)
            translation_match = re.match(r'\s*([a-z\-]+) \"(.*)\"', line)
            key, value = obtain_key_value_from_translation_line(translation_match,line)
            # add the key-value pair to the section list
            if key != '#':
                section_list.append([key, value])
                prev_key = key
            elif '#' in value:
                # Current line in file is a comment, we want to keep
                # track of its location in the original file
                comment_list.append(current_line_loc)
            current_line_loc += 1
        section_list_with_attributes = SectionList(section_list)
        section_list_with_attributes.comment_locations = comment_list.copy()
        
        # add the section list to the main list
        data_dict[title] = section_list_with_attributes
    return data_dict

def single_file_reader(input_file_name):
    with open(input_file_name, 'r') as input_file:
        for line in input_file:
            yield line

def single_file_writer(output_file_name):
    with open(output_file_name, 'w') as output_file:
        while True:
            line = yield
            output_file.write(line)

def double_file_reader(file1, file2):
    with open(file1, 'r') as f1, open(file2, 'r') as f2:
        for line1, line2 in zip(f1, f2):
            yield (line1, line2)

def detect_language(file_name):
    with open(file_name, 'r') as f:
        first_line = f.readline()
        lang = first_line.split()[0]
        return lang

def detect_leading_whitespace_from_source_lang_file(file_name):
    with open(file_name, 'r') as f:
        first_line = f.readline()
        whitespace = first_line[:len(first_line) - len(first_line.lstrip())]
        return whitespace

@dataclass
class StateControlData:
    """ Class for keeping track of state machine information"""
    current_state: str = ''
    current_header: str = ''
    detected_dest_lang: str = ''
    whitespace: str = ''
    insert_point_index: int = 0
    stop_state_machine: bool = False
    mapped_input_data: any = None
    input_reader: any = None
    output_writer: any = None
    eng_to_new_lang_translation_mapper: any = None
    
    
def searching_for_next_header_action(state_machine_data):
    for input_line in state_machine_data.input_reader:
        if re.match(r'^[A-Z]+', input_line):
            state_machine_data.current_header = input_line.strip()
            state_machine_data.current_state = "CALCULATE_INSERT_POINT"
            state_machine_data.output_writer.send(input_line)
            break
        state_machine_data.output_writer.send(input_line)
    else:
        state_machine_data.stop_state_machine = True
    
    return state_machine_data

def calculate_insert_point_action(state_machine_data):
    detected_dest_lang = state_machine_data.detected_dest_lang
    current_header = state_machine_data.current_header

    old_lang_list = state_machine_data.mapped_input_data[current_header]

    # Determine the spot where the new translation should fit in
    # the list of translations
    index = bisect.bisect([lang for lang, _ in old_lang_list], detected_dest_lang)
    
    state_machine_data.insert_point_index = index
    state_machine_data.current_state = "PERFORM_INSERT"
    
    return state_machine_data

def finding_insert_point_action(state_machine_data):
    def adjust_for_comments_occuring_before_insert_point(insert_point_index, comment_locations):
        for comment_loc in comment_locations:
            if comment_loc <= insert_point_index:
                insert_point_index += 1
        return insert_point_index
            
    eng_to_new_lang_tuple = next(state_machine_data.eng_to_new_lang_translation_mapper)
    current_header = state_machine_data.current_header
    old_lang_list = state_machine_data.mapped_input_data[current_header]
    index = adjust_for_comments_occuring_before_insert_point(state_machine_data.insert_point_index, old_lang_list.comment_locations)
    detected_whitespace = state_machine_data.whitespace

    
    for i,elem in enumerate(old_lang_list):
        if index == i:
            state_machine_data.output_writer.send(detected_whitespace + eng_to_new_lang_tuple[1])
        
        input_line = next(state_machine_data.input_reader, None)
        if input_line is None:
            pdb.set_trace()
        state_machine_data.output_writer.send(input_line) 
        
    
    # New lang should be placed last
    if index >= len(old_lang_list):    
        state_machine_data.output_writer.send(detected_whitespace + eng_to_new_lang_tuple[1])
        #state_machine_data.output_writer.send("\n") # The lines are stripped so we add a carriage-return

    state_machine_data.current_state = "SEARCHING_FOR_NEXT_HEADER"
    return state_machine_data
        

def language_inserter(data_dict, english_lang_translations_file, new_lang_translations_file):
    '''
    Inserts the new language into a copy of errmsg-utf8.txt, using a state machine to
    keep track of what step it is to take. Coroutines are used to keep control flow
    tractable when dealing with 4 separate files
    '''
    state_machine = {
        "SEARCHING_FOR_NEXT_HEADER" : searching_for_next_header_action,
        "CALCULATE_INSERT_POINT" : calculate_insert_point_action,
        "PERFORM_INSERT" : finding_insert_point_action
    }

    state_machine_data = StateControlData()
    
    state_machine_data.output_writer = single_file_writer('errmsg-utf8-with-new-language.txt')
    next(state_machine_data.output_writer)
    state_machine_data.input_reader = single_file_reader('errmsg-utf8.txt')
    state_machine_data.eng_to_new_lang_translation_mapper = double_file_reader(english_lang_translations_file, new_lang_translations_file)

    state_machine_data.detected_dest_lang = detect_language(new_lang_translations_file)
    state_machine_data.whitespace = detect_leading_whitespace_from_source_lang_file(english_lang_translations_file)
    state_machine_data.current_header =''
    state_machine_data.current_state = "SEARCHING_FOR_NEXT_HEADER"
    state_machine_data.mapped_input_data = data_dict

    while not state_machine_data.stop_state_machine:
        current_state = state_machine_data.current_state
        state_machine_data = state_machine[current_state](state_machine_data)
        

def main():
    ''' main function '''
    parser = argparse.ArgumentParser(description='''Given errmsg-utf8.txt, 
    an english language file extracted from errmsg-utf8.txt and another
    file with translations into a new language from the english language
    file, reinsert the new language translations into their correct
    positions in a copy of errmsg-utf8.txt.''')
    parser.add_argument('errmsg_file', type=str, help='Path to errmsg-utf8.txt')
    parser.add_argument('english_lang_translations_file', type=str, help='Path to English lang translations file')
    parser.add_argument('new_lang_translations_file', type=str, help='Path to new lang translations file')

    args = parser.parse_args()
    errmsg_file = args.errmsg_file
    english_lang_translations_file = args.english_lang_translations_file
    new_lang_translations_file = args.new_lang_translations_file
    
    data = read_file(errmsg_file)
    data_dict = map_out_source_data(data)
    print('Original file errmsg-utf8.txt has been successfully mapped into memory.')
    print('''Now starting insertion process into errmsg-utf8-with-new-language.txt which is
 a copy of errmsg-utf8.txt''')

    # In case you want to hard code the language source files, uncomment
    # the below two lines, set the new language file name and disable
    # argument parsing.
    #english_lang_translations_file = 'all_english_text_in_errmsg-utf8.txt'
    #new_lang_translations_file = 'all_swahili_text_in_errmsg-utf8.txt'
    language_inserter(data_dict, english_lang_translations_file, new_lang_translations_file)
    print("Insertion of new language translations into errmsg-utf8-with-new-language.txt is done")

# call the main function
if __name__ == "__main__":
    main()



