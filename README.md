# rgrep — рекурсивный поиск слова по каталогу (Linux)

Сборка:
  gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -O2 -o rgrep src/rgrep.c

Запуск:
  ./rgrep "word"            # ищет в ~/files
  ./rgrep -i DIR "Word"     # игнор регистра
  ./rgrep --mmap . word     # чтение через mmap

Вывод: путь:номер_строки:строка. Скрытые файлы/каталоги учитываются.
