package main

import (
	"encoding/json"
	"fmt"
	"log"

	"./repo"

	"github.com/buaazp/fasthttprouter"
	"github.com/valyala/fasthttp"
)

func QueryGetByIDHandler(ctx *fasthttp.RequestCtx) {
	r := ctx.UserValue("repo").(string)
	item := repo.Get(r).QueryByID(1, false)
	ret, _ := json.Marshal(item)
	ctx.Write(ret)
}

func Query1CondHandler(ctx *fasthttp.RequestCtx) {
	r := ctx.UserValue("repo").(string)
	items := repo.Get(r).Query1Cond(1, false, 10)
	ret, _ := json.Marshal(items)
	ctx.Write(ret)
}

func Query2CondHandler(ctx *fasthttp.RequestCtx) {
	r := ctx.UserValue("repo").(string)
	items := repo.Get(r).Query2Cond(1, false, 10)
	ret, _ := json.Marshal(items)
	ctx.Write(ret)
}

func randStringWord() string { return repo.RandStringWord() }

func randStringPref() string {
	s := randStringWord()
	if len(s) > 4 {
		s = s[:len(s)-2]
	}
	return s
}

func QueryTextHandler(ctx *fasthttp.RequestCtx) {
	r := ctx.UserValue("repo").(string)

	dsl := ""
	switch r {
	case "elastic", "mysql", "reindex":
		dsl = "%s %s"
	case "sqlite":
		dsl = "%s OR %s"
	case "sphinx":
		dsl = "%s | %s"
	case "mongo":
		dsl = "%s %s"
	}

	items := repo.Get(r).QueryFullText(func() string { return fmt.Sprintf(dsl, randStringWord(), randStringWord()) }, 1, 10)
	ret, _ := json.Marshal(items)
	ctx.Write(ret)
}

func QueryTextPrefixHandler(ctx *fasthttp.RequestCtx) {
	r := ctx.UserValue("repo").(string)

	dsl := ""
	switch r {
	case "elastic", "mysql", "reindex":
		dsl = "%s* %s*"
	case "sqlite":
		dsl = "%s* OR %s*"
	case "sphinx":
		dsl = "%s* | %s*"
	}

	items := repo.Get(r).QueryFullText(func() string { return fmt.Sprintf(dsl, randStringPref(), randStringPref()) }, 1, 10)
	ret, _ := json.Marshal(items)
	ctx.Write(ret)
}

func UpdateHandler(ctx *fasthttp.RequestCtx) {
	r := ctx.UserValue("repo").(string)
	repo.Get(r).Update(1)
	ctx.WriteString("{}")
}

func StartHTTP() {
	router := fasthttprouter.New()

	router.GET("/byid/:repo", QueryGetByIDHandler)
	router.GET("/1cond/:repo", Query1CondHandler)
	router.GET("/2cond/:repo", Query2CondHandler)
	router.GET("/text/:repo", QueryTextHandler)
	router.GET("/text_prefix/:repo", QueryTextPrefixHandler)
	router.GET("/update/:repo", UpdateHandler)

	log.Printf("Starting listen")
	fasthttp.ListenAndServe(":8080", router.Handler)
}
