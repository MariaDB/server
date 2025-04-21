## A quicker way for adding new language translations to the errmsg-utf8.txt file

### Summary

To generate a new language translation of MariaDB use the following pull request (PR) as a template for your work:
- https://github.com/MariaDB/server/pull/2676

You will notice as part of your translation work, you will have to add your language translations to the file `sql/share/errmsg-utf8.txt` which is found in the current directory. This file is long with many sections which can make the translation work tedious. In this README, we explain a procedure and provide a script `insert_translations_into_errmsg.py` that cuts down the amount of tedium in accomplishing the task.

### Procedure
1. Start by grepping out all the english translations from errmsg-utf8.txt using the following grep command, and redirecting the output to a file:

    grep -P "^\s*eng\s" errmsg-utf8.txt > all_english_text_in_errmsg-utf8.txt

2. Next use Google translate to obtain a translation of this file. Google translate provides the ability to upload whole files for translation. For example, this technique was used to obtain Swahili translations which yielded a file with output similar to the below (output is truncated for clarity):

    sw "hashchk"
    sw "isamchk"
    sw "LA"
    sw "NDIYO"
    sw "Haiwezi kuunda faili '% -.200s' (kosa: %iE)"
    sw "Haiwezi kuunda jedwali %`s.%`s (kosa: %iE)"
    sw "Haiwezi kuunda hifadhidata '% -.192s' (kosa: %iE)"
    sw "Haiwezi kuunda hifadhidata '% -.192s'; hifadhidata ipo"

Note that Google translate removes the leading whitespace in the translation file it generates. DO NOT add that leading whitespace back!

3. Give the translated file an appropriate name (e.g. `all_swahili_text_in_errmsg-utf8.txt`) and store it in the same directory with `errmsg-utf8.txt` and `all_english_text_in_errmsg-utf8.txt`. These 3 files will be used by the script insert_translations_into_errmsg.py.

4. Proof check the auto-translations in the file you downloaded from Google translate. Note that Google might omit formatting information
that will cause the compilation of MariaDB to fail, so pay attention to these.

5. Reintegrate these translations into the errmsg-utf8.txt by running the insert_translations_into_errmsg.py script as follows:

    chmod ugo+x insert_translations_into_errmsg.py # Make the script executable if it is not.
    
    ./insert_translations_into_errmsg.py <errmsg-utf8.txt file> <file with grepped english entries> <file with new language entries>

   For example, for the swahili translation, we ran the following:
   
    ./insert_translations_into_errmsg.py errmsg-utf8.txt all_english_text_in_errmsg-utf8.txt all_swahili_text_in_errmsg-utf8.txt

   The script uses the `errmsg-utf8.txt` file and the grepped english file to keep track of each new translation. It then creates a file in the same directory as `errmsg-utf8.txt` with the name `errmsg-utf8-with-new-language.txt`.

6. Check that the reintegration of the new translations into `errmsg-utf8-with-new-language.txt` went OK, and if it did, rename `errmsg-utf8-with-new-language.txt` to `errmsg-utf8.txt`:

    mv errmsg-utf8-with-new-language.txt errmsg-utf8.txt

7. In the header of errmsg-utf8.txt make sure to add your language long form to short form mapping. E.g. for Swahili, add:

    swahili=sw
