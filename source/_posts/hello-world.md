---
title: Hello World
mathjax: true
---
Welcome to [Hexo](https://hexo.io/)! This is your very first post. Check [documentation](https://hexo.io/docs/) for more info. If you get any problems when using Hexo, you can find the answer in [troubleshooting](https://hexo.io/docs/troubleshooting.html) or you can ask me on [GitHub](https://github.com/hexojs/hexo/issues).

## Quick Start

### LaTeX test

$$\int_{0}^{1}\!x^{-x} \mathrm{d}x = \sum_{n=1}^{\infty}n^{-n}$$

### Code test

```ocaml
(* take first n elements from list xs *)
let take n xs =
  let rec take_impl acc n xs = match (n, xs) with
    | (0, _) -> acc
    | (_, []) -> acc
    | (n, x::xs) -> take_impl (x::acc) (n-1) xs
  in List.rev (take_impl [] n xs)

(* drop first n elements from list xs *)
let rec drop n xs = match (n, xs) with
  | (_, []) -> []
  | (0, xs) -> xs
  | (n, _::xs) -> drop (n-1) xs
```

### Create a new post

``` bash
$ hexo new "My New Post"
```

More info: [Writing](https://hexo.io/docs/writing.html)

### Run server

``` bash
$ hexo server
```

More info: [Server](https://hexo.io/docs/server.html)

### Generate static files

``` bash
$ hexo generate
```

More info: [Generating](https://hexo.io/docs/generating.html)

### Deploy to remote sites

``` bash
$ hexo deploy
```

More info: [Deployment](https://hexo.io/docs/one-command-deployment.html)
