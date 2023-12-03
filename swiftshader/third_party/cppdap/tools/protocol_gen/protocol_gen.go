// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// protocol_gen (re)generates the cppdap .h and .cpp files that describe the
// DAP protocol.
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"io/ioutil"
	"net/http"
	"os"
	"os/exec"
	"path"
	"reflect"
	"runtime"
	"sort"
	"strings"
)

var (
	cache = flag.String("cache", "", "File cache of the .json schema")
)

const (
	jsonURL = "https://raw.githubusercontent.com/microsoft/vscode-debugadapter-node/master/debugProtocol.json"

	commonPrologue = `// Copyright 2019 Google LLC
	//
	// Licensed under the Apache License, Version 2.0 (the "License");
	// you may not use this file except in compliance with the License.
	// You may obtain a copy of the License at
	//
	//     https://www.apache.org/licenses/LICENSE-2.0
	//
	// Unless required by applicable law or agreed to in writing, software
	// distributed under the License is distributed on an "AS IS" BASIS,
	// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
	// See the License for the specific language governing permissions and
	// limitations under the License.

	// Generated with protocol_gen.go -- do not edit this file.
	//   go run scripts/protocol_gen/protocol_gen.go
`

	headerPrologue = commonPrologue + `
#ifndef dap_protocol_h
#define dap_protocol_h

#include "optional.h"
#include "typeinfo.h"
#include "typeof.h"
#include "variant.h"

#include <string>
#include <type_traits>
#include <vector>

namespace dap {

struct Request {};
struct Response {};
struct Event {};

`

	headerEpilogue = `}  // namespace dap

#endif  // dap_protocol_h
`

	cppPrologue = commonPrologue + `

#include "dap/protocol.h"

namespace dap {

`

	cppEpilogue = `}  // namespace dap
`
)

func main() {
	flag.Parse()
	if err := run(); err != nil {
		fmt.Fprintf(os.Stderr, "%v\n", err)
		os.Exit(1)
	}
}

type root struct {
	Schema      string                `json:"$schema"`
	Title       string                `json:"title"`
	Description string                `json:"description"`
	Ty          string                `json:"type"`
	Definitions map[string]definition `json:"definitions"`
}

func (r *root) definitions() []namedDefinition {
	sortedDefinitions := make([]namedDefinition, 0, len(r.Definitions))
	for name, def := range r.Definitions {
		sortedDefinitions = append(sortedDefinitions, namedDefinition{name, def})
	}
	sort.Slice(sortedDefinitions, func(i, j int) bool { return sortedDefinitions[i].name < sortedDefinitions[j].name })
	return sortedDefinitions
}

func (r *root) getRef(ref string) (namedDefinition, error) {
	if !strings.HasPrefix(ref, "#/definitions/") {
		return namedDefinition{}, fmt.Errorf("Unknown $ref '%s'", ref)
	}
	name := strings.TrimPrefix(ref, "#/definitions/")
	def, ok := r.Definitions[name]
	if !ok {
		return namedDefinition{}, fmt.Errorf("Unknown $ref '%s'", ref)
	}
	return namedDefinition{name, def}, nil
}

type namedDefinition struct {
	name string
	def  definition
}

type definition struct {
	Ty          string       `json:"type"`
	Title       string       `json:"title"`
	Description string       `json:"description"`
	Properties  properties   `json:"properties"`
	Required    []string     `json:"required"`
	AllOf       []definition `json:"allOf"`
	Ref         string       `json:"$ref"`
}

type properties map[string]property

func (p *properties) foreach(cb func(string, property) error) error {
	type namedProperty struct {
		name     string
		property property
	}
	sorted := make([]namedProperty, 0, len(*p))
	for name, property := range *p {
		sorted = append(sorted, namedProperty{name, property})
	}
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].name < sorted[j].name })
	for _, entry := range sorted {
		if err := cb(entry.name, entry.property); err != nil {
			return err
		}
	}
	return nil
}

type property struct {
	typed
	Description string `json:"description"`
}

func (p *property) properties(r *root) (properties, []string, error) {
	if p.Ref == "" {
		return p.Properties, p.Required, nil
	}

	d, err := r.getRef(p.Ref)
	if err != nil {
		return nil, nil, err
	}
	return d.def.Properties, d.def.Required, nil
}

type typed struct {
	Ty         interface{} `json:"type"`
	Items      *typed      `json:"items"`
	Ref        string      `json:"$ref"`
	Properties properties  `json:"properties"`
	Required   []string    `json:"required"`
	ClosedEnum []string    `json:"enum"`
	OpenEnum   []string    `json:"_enum"`
}

func (t typed) typename(r *root, refs *[]string) (string, error) {
	if t.Ref != "" {
		d, err := r.getRef(t.Ref)
		if err != nil {
			return "", err
		}
		*refs = append(*refs, d.name)
		return d.name, nil
	}

	if t.Ty == nil {
		return "", fmt.Errorf("No type specified")
	}

	var typeof func(v reflect.Value) (string, error)
	typeof = func(v reflect.Value) (string, error) {
		if v.Kind() == reflect.Interface {
			v = v.Elem()
		}
		switch v.Kind() {
		case reflect.String:
			ty := v.Interface().(string)
			switch ty {
			case "boolean", "string", "integer", "number", "object", "null":
				return ty, nil
			case "array":
				if t.Items != nil {
					el, err := t.Items.typename(r, refs)
					if err != nil {
						return "", err
					}
					return fmt.Sprintf("array<%s>", el), nil
				}
				return "array<any>", nil
			default:
				return "", fmt.Errorf("Unhandled property type '%v'", ty)
			}

		case reflect.Slice, reflect.Array:
			ty := "variant<"
			for i := 0; i < v.Len(); i++ {
				if i > 0 {
					ty += ", "
				}
				el, err := typeof(v.Index(i))
				if err != nil {
					return "", err
				}
				ty += el
			}
			ty += ">"
			return ty, nil
		}

		return "", fmt.Errorf("Unsupported type '%v' kind: %v", v.Interface(), v.Kind())
	}

	return typeof(reflect.ValueOf(t.Ty))
}

type cppField struct {
	desc         string
	ty           string
	name         string
	defaultValue string
	optional     bool
}

type cppStruct struct {
	desc     string
	name     string
	typename string
	base     string
	fields   []cppField
	deps     []string
	emit     bool
	typedefs []cppTypedef
	ty       structType
}

type cppTypedef struct {
	from string
	to   string
}

func sanitize(s string) string {
	s = strings.Trim(s, "_")
	switch s {
	case "default":
		return "def"
	default:
		return s
	}
}

func (s *cppStruct) writeHeader(w io.Writer) {
	if s.desc != "" {
		io.WriteString(w, "// ")
		io.WriteString(w, strings.ReplaceAll(s.desc, "\n", "\n// "))
		io.WriteString(w, "\n")
	}
	io.WriteString(w, "struct ")
	io.WriteString(w, s.name)
	if s.base != "" {
		io.WriteString(w, " : public ")
		io.WriteString(w, s.base)
	}
	io.WriteString(w, " {")

	// typedefs
	for _, t := range s.typedefs {
		io.WriteString(w, "\n  using ")
		io.WriteString(w, t.from)
		io.WriteString(w, " = ")
		io.WriteString(w, t.to)
		io.WriteString(w, ";")
	}

	// constructor
	io.WriteString(w, "\n\n  ")
	io.WriteString(w, s.name)
	io.WriteString(w, "();")

	// destructor
	io.WriteString(w, "\n  ~")
	io.WriteString(w, s.name)
	io.WriteString(w, "();\n")

	for _, f := range s.fields {
		if f.desc != "" {
			io.WriteString(w, "\n  // ")
			io.WriteString(w, strings.ReplaceAll(f.desc, "\n", "\n  // "))
		}
		io.WriteString(w, "\n  ")
		if f.optional {
			io.WriteString(w, "optional<")
			io.WriteString(w, f.ty)
			io.WriteString(w, ">")
		} else {
			io.WriteString(w, f.ty)
		}
		io.WriteString(w, " ")
		io.WriteString(w, sanitize(f.name))
		if !f.optional && f.defaultValue != "" {
			io.WriteString(w, " = ")
			io.WriteString(w, f.defaultValue)
		}
		io.WriteString(w, ";")
	}

	io.WriteString(w, "\n};\n\n")

	io.WriteString(w, "DAP_DECLARE_STRUCT_TYPEINFO(")
	io.WriteString(w, s.name)
	io.WriteString(w, ");\n\n")
}

func (s *cppStruct) writeCPP(w io.Writer) {
	// constructor
	io.WriteString(w, s.name)
	io.WriteString(w, "::")
	io.WriteString(w, s.name)
	io.WriteString(w, "() = default;\n")

	// destructor
	io.WriteString(w, s.name)
	io.WriteString(w, "::~")
	io.WriteString(w, s.name)
	io.WriteString(w, "() = default;\n")

	// typeinfo
	io.WriteString(w, "DAP_IMPLEMENT_STRUCT_TYPEINFO(")
	io.WriteString(w, s.name)
	io.WriteString(w, ",\n                    \"")
	io.WriteString(w, s.typename)
	io.WriteString(w, "\"")
	for _, f := range s.fields {
		io.WriteString(w, ",\n                    ")
		io.WriteString(w, "DAP_FIELD(")
		io.WriteString(w, sanitize(f.name))
		io.WriteString(w, ", \"")
		io.WriteString(w, f.name)
		io.WriteString(w, "\")")
	}
	io.WriteString(w, ");\n\n")
}

func buildStructs(r *root) ([]*cppStruct, error) {
	ignore := map[string]bool{
		// These are handled internally.
		"ProtocolMessage": true,
		"Request":         true,
		"Event":           true,
		"Response":        true,
	}

	out := []*cppStruct{}
	for _, entry := range r.definitions() {
		defName, def := entry.name, entry.def
		if ignore[defName] {
			continue
		}

		base := ""
		if len(def.AllOf) > 1 && def.AllOf[0].Ref != "" {
			ref, err := r.getRef(def.AllOf[0].Ref)
			if err != nil {
				return nil, err
			}
			base = ref.name
			if len(def.AllOf) > 2 {
				return nil, fmt.Errorf("Cannot handle allOf with more than 2 entries")
			}
			def = def.AllOf[1]
		}

		s := cppStruct{
			desc: def.Description,
			name: defName,
			base: base,
		}

		var props properties
		var required []string
		var err error
		switch base {
		case "Request":
			if arguments, ok := def.Properties["arguments"]; ok {
				props, required, err = arguments.properties(r)
			}
			if command, ok := def.Properties["command"]; ok {
				s.typename = command.ClosedEnum[0]
			}
			response := strings.TrimSuffix(s.name, "Request") + "Response"
			s.deps = append(s.deps, response)
			s.typedefs = append(s.typedefs, cppTypedef{"Response", response})
			s.emit = true
			s.ty = request
		case "Response":
			if body, ok := def.Properties["body"]; ok {
				props, required, err = body.properties(r)
			}
			s.emit = true
			s.ty = response
		case "Event":
			if body, ok := def.Properties["body"]; ok {
				props, required, err = body.properties(r)
			}
			if command, ok := def.Properties["event"]; ok {
				s.typename = command.ClosedEnum[0]
			}
			s.emit = true
			s.ty = event
		default:
			props = def.Properties
			required = def.Required
			s.ty = types
		}
		if err != nil {
			return nil, err
		}

		if err = props.foreach(func(propName string, property property) error {
			ty, err := property.typename(r, &s.deps)
			if err != nil {
				return fmt.Errorf("While processing %v.%v: %v", defName, propName, err)
			}

			optional := true
			for _, r := range required {
				if propName == r {
					optional = false
				}
			}

			desc := property.Description
			defaultValue := ""

			if len(property.ClosedEnum) > 0 {
				desc += "\n\nMust be one of the following enumeration values:\n"
				for i, enum := range property.ClosedEnum {
					if i > 0 {
						desc += ", "
					}
					desc += "'" + enum + "'"
				}
				defaultValue = `"` + property.ClosedEnum[0] + `"`
			}

			if len(property.OpenEnum) > 0 {
				desc += "\n\nMay be one of the following enumeration values:\n"
				for i, enum := range property.OpenEnum {
					if i > 0 {
						desc += ", "
					}
					desc += "'" + enum + "'"
				}
			}

			s.fields = append(s.fields, cppField{
				desc:         desc,
				defaultValue: defaultValue,
				ty:           ty,
				name:         propName,
				optional:     optional,
			})

			return nil
		}); err != nil {
			return nil, err
		}

		out = append(out, &s)
	}

	return out, nil
}

type structType string

const (
	request  = structType("request")
	response = structType("response")
	event    = structType("event")
	types    = structType("types")
)

type cppFilePaths map[structType]string

type cppFiles map[structType]*os.File

func run() error {
	data, err := loadJSONFile()
	if err != nil {
		return err
	}
	r := root{}
	d := json.NewDecoder(bytes.NewReader(data))
	if err := d.Decode(&r); err != nil {
		return err
	}

	hPath, cppPaths := outputPaths()
	if err := emitFiles(&r, hPath, cppPaths); err != nil {
		return err
	}

	if clangfmt, err := exec.LookPath("clang-format"); err == nil {
		if err := exec.Command(clangfmt, "-i", hPath).Run(); err != nil {
			return err
		}
		for _, p := range cppPaths {
			if err := exec.Command(clangfmt, "-i", p).Run(); err != nil {
				return err
			}
		}
	}

	return nil
}

func emitFiles(r *root, hPath string, cppPaths map[structType]string) error {
	h, err := os.Create(hPath)
	if err != nil {
		return err
	}
	defer h.Close()
	cppFiles := map[structType]*os.File{}
	for ty, p := range cppPaths {
		f, err := os.Create(p)
		if err != nil {
			return err
		}
		cppFiles[ty] = f
		defer f.Close()
	}

	h.WriteString(headerPrologue)
	for _, f := range cppFiles {
		f.WriteString(cppPrologue)
	}

	structs, err := buildStructs(r)
	if err != nil {
		return err
	}

	structsByName := map[string]*cppStruct{}
	for _, s := range structs {
		structsByName[s.name] = s
	}

	seen := map[string]bool{}
	var emit func(*cppStruct)
	emit = func(s *cppStruct) {
		if seen[s.name] {
			return
		}
		seen[s.name] = true
		for _, dep := range s.deps {
			emit(structsByName[dep])
		}
		s.writeHeader(h)
		s.writeCPP(cppFiles[s.ty])
	}

	// emit message types.
	// Referenced structs will be transitively emitted.
	for _, s := range structs {
		switch s.ty {
		case request, response, event:
			emit(s)
		}
	}

	h.WriteString(headerEpilogue)
	for _, f := range cppFiles {
		f.WriteString(cppEpilogue)
	}

	return nil
}

func loadJSONFile() ([]byte, error) {
	if *cache != "" {
		data, err := ioutil.ReadFile(*cache)
		if err == nil {
			return data, nil
		}
	}
	resp, err := http.Get(jsonURL)
	if err != nil {
		return nil, err
	}
	data, err := ioutil.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	if *cache != "" {
		ioutil.WriteFile(*cache, data, 0777)
	}
	return data, nil
}

func outputPaths() (string, cppFilePaths) {
	_, thisFile, _, _ := runtime.Caller(1)
	thisDir := path.Dir(thisFile)
	h := path.Join(thisDir, "../../include/dap/protocol.h")
	cpp := cppFilePaths{
		request:  path.Join(thisDir, "../../src/protocol_requests.cpp"),
		response: path.Join(thisDir, "../../src/protocol_response.cpp"),
		event:    path.Join(thisDir, "../../src/protocol_events.cpp"),
		types:    path.Join(thisDir, "../../src/protocol_types.cpp"),
	}
	return h, cpp
}