 
# mod_libmemcached_deny_blacklist.c

## description

memcachedのキャッシュの有無で接続の許可/拒否をくだすproftpdモジュール

## usage

memcachedを127.0.0.1で起動して用いるベーシックな使い方

```
        LoadModule mod_libmemcached_deny.c
        <IfModule mod_libmemcached_deny.c>
           LMDBMemcachedHost 127.0.0.1
        </IfModule>
```

ログインを禁止したい場合はアカウント名かIPアドレスをキーにしてストアすれば良いです
    