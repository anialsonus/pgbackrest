Данная инструкция описывает, как скомпилировать pgBackRest, создать с его помощью резервную копию кластера Greenplum и восстановиться из нее.

# 1. Поддерживаемые версии Greenplum

На данный момент поддерживается только [Greenplum 6](https://docs.vmware.com/en/VMware-Greenplum/6/greenplum-database/landing-index.html).

# 2. Компиляция pgBackRest

- Установить зависимости

Приведенные ниже команды требуется выполнить от имени суперпользователя.

для CentOS 7:
```
yum install git gcc openssl-devel libxml2-devel bzip2-devel libzstd-devel lz4-devel libyaml-devel
```

для ALT Linux p10
```
apt-get update
apt-get install git gcc openssl-devel libxml2-devel bzip2-devel libzstd-devel libyaml-devel zlib-devel 
```

- Добавить в переменную окружения PATH путь к pg_config Greenplum

- Скачать репозиторий
```
git clone https://github.com/arenadata/pgbackrest -b 2.50-ci
```

- Перейти в каталог с исходным кодом
```
cd pgbackrest/src/
```

- Запустить скрипт конфигурирования

Без параметров будут включены оптимизация (-O2) и векторизация вычисления контрольных сумм страниц.
```
./configure
```

Конфигурирование для отладки рекомендуется выполнять командой
```
CFLAGS="-O0 -g3" ./configure --disable-optimize --enable-test
```

- Собрать

Команда для сборки с использованием всех доступных ядер и без отображения исполняемых команд
```
make -j`nproc` -s
```

В результате в текущем каталоге появится исполняемый файл с именем `pgbackrest`.

- Установить

```
sudo make install
```
По умолчанию исполняемый файл `pgbackrest` будет размещен в каталог `/usr/local/bin`. Это единственное действие, которое выполняется на этапе установки.


# 3. Настройка
- Создать каталоги для размещения бэкапа и логов. Пусть для теста это будут `/tmp/backup` и `/tmp/backup/log`.
```
mkdir -p /tmp/backup/log
```

- Создать конфигурационный файл

В дальнейших примерах команд предполагается, что конфигурационный файл имеет стандартное имя - `/etc/pgbackrest.conf`. Если требуется использовать другой файл, то его имя можно передать через параметр `--config`. Для **стандартного демонстрационного кластера**, созданного с `DATADIRS=/tmp/gpdb`, команда создания конфигурационного файла потребует права суперпользователя и будет выглядеть так:
```
cat <<EOF > /etc/pgbackrest.conf
[seg-1]
pg1-path=/tmp/gpdb/qddir/demoDataDir-1
pg1-port=6000

[seg0]
pg1-path=/tmp/gpdb/dbfast1/demoDataDir0
pg1-port=6002

[seg1]
pg1-path=/tmp/gpdb/dbfast2/demoDataDir1
pg1-port=6003

[seg2]
pg1-path=/tmp/gpdb/dbfast3/demoDataDir2
pg1-port=6004

[global]
repo1-path=/tmp/backup
log-path=/tmp/backup/log
start-fast=y
fork=GPDB
EOF
```

Так как данная версия pgBackRest может применяться для бэкапа как PostgreSQL, так и Greenplum, следует указать в параметре `fork`, бэкап какой СУБД выполняется. Описание остальных параметров можно найти в [документации](https://pgbackrest.org/configuration.html) или в `build/help/help.xml`.

- Создать и инициализировать для координатора и каждого первичного сегмента каталоги, в которых будут храниться файлы для восстановления
```
for i in -1 0 1 2
do 
    PGOPTIONS="-c gp_session_role=utility" pgbackrest --stanza=seg$i stanza-create
done
```
Под термином "stanza" понимается "строфа" в конфигурационном файле

- Настроить Greenplum
```
gpconfig -c archive_mode -v on
gpconfig -c archive_command -v "'PGOPTIONS=\"-c gp_session_role=utility\" /usr/local/bin/pgbackrest --stanza=seg%c archive-push %p'" --skipvalidation
gpstop -ar
```

- Установить расширение gp_pitr

Выполнить приведенный ниже запрос в любом клиентском приложении, например в psql.
```
create extension gp_pitr;
```
Это расширение необходимо для работы с точками восстановления.

- Проверить, что резервное копирование и архивирование логов предзаписи правильно настроено
```
for i in -1 0 1 2
do 
	PGOPTIONS="-c gp_session_role=utility" pgbackrest --stanza=seg$i check
done
```
Команда не должна выдать сообщений об ошибке

# 4. Резервное копирование кластера Greenplum

4.1 Сохранить файлы с первичных сегментов и координатора
```
for i in -1 0 1 2
do 
    PGOPTIONS="-c gp_session_role=utility" pgbackrest --stanza=seg$i backup
done
```
Если потребуется остановить прерванный бэкап, то это можно сделать запросом
```
select pg_stop_backup() from gp_dist_random('gp_id');
```

4.2 Создать именованную точку восстановления

Выполнить запрос
```
select gp_create_restore_point('backup1');
```
Запись о точке восстановления будет помещена в лог предзаписи.

4.3 Отправить в архив файлы, хранящие созданную точку восстановления.

Отправка осуществляется через переключение логов предзаписи координатора и сегментов на новые файлы с помощью запроса
```
select gp_switch_wal();
```

# 5. Восстановление кластера Greenplum

- Выключить кластер и удалить содержимое каталогов всех компонентов кластера
```
gpstop -a
rm -rf /tmp/gpdb/qddir/demoDataDir-1/* /tmp/gpdb/dbfast1/demoDataDir0/* /tmp/gpdb/dbfast2/demoDataDir1/* /tmp/gpdb/dbfast3/demoDataDir2/* /tmp/gpdb/dbfast_mirror1/demoDataDir0/* /tmp/gpdb/dbfast_mirror2/demoDataDir1/* /tmp/gpdb/dbfast_mirror3/demoDataDir2/* /tmp/gpdb/standby/*
```

- Восстановить из резервной копии содержимое каталогов координатора и первичных сегментов

Имя точки восстановления из пункта 4.2 передается в параметре `--target`.
```
for i in -1 0 1 2
do 
    pgbackrest --stanza=seg$i --type=name --target=backup1 restore
done
```

- Запустить только координатор
```
gpstart -am
```

- Удалить резервный координатор из кластера
```
gpinitstandby -ar
```

- Отметить зеркала сегментов как недоступные
```
PGOPTIONS="-c gp_session_role=utility" psql postgres -c "
set allow_system_table_mods to true;
update gp_segment_configuration 
 set status = case when role='m' then 'd' else status end, 
     mode = 'n'
 where content >= 0;"
```

- Перезапустить кластер в обычном режиме
```
gpstop -ar
```

- Восстановить зеркала по первичным сегментам
```
gprecoverseg -aF
```

- Восстановить резервный координатор
```
gpinitstandby -as $HOSTNAME -S /tmp/gpdb/standby -P 6001
```

- Убедиться, что все компоненты кластера восстановились и работают

Выполнить запрос
```
select * from gp_segment_configuration order by content, role;
```
