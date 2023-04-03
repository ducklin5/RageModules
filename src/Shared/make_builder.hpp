/*
  * MIT License
  * 
  * Copyright (c) 2023 Azeez Abass
  * 
  * Permission is hereby granted, free of charge, to any person obtaining a copy
  * of this software and associated documentation files (the "Software"), to deal
  * in the Software without restriction, including without limitation the rights
  * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  * copies of the Software, and to permit persons to whom the Software is
  * furnished to do so, subject to the following conditions:
  * 
  * The above copyright notice and this permission notice shall be included in all
  * copies or substantial portions of the Software.
  * 
  * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  * SOFTWARE.
 */

/* 
//// Usage:
#include <stdio.h>
struct Foo {
    int bar = 8;
    float car = 2.0;
    char* dar;
};


MAKE_BUILDER(MakeFoo, Foo, bar, car, dar);
int main() {
    char str[] = "foobar";
    Foo obj = MakeFoo()
        .car(3.14)
        .dar(str);
    printf("obj { bar:%i, car:%f, dar:%s }\n", obj.bar, obj.car, obj.dar);
    return 0;
}
*/

#ifndef MAKE_BUILDER_H_INCLUDED
#define MAKE_BUILDER_H_INCLUDED

// ---------- MAP MACRO FUNCTION -----------
// source: https://stackoverflow.com/questions/6707148/foreach-macro-on-macros-arguments/13459454#13459454
// alt: https://github.com/swansontec/map-macro/tree/master 
// by: William Swanson [THE GOAT]
// mod: Azeez Abass (modified to pass an additonal options param to each iteration) 

#define EVAL0(...) __VA_ARGS__
#define EVAL1(...) EVAL0(EVAL0(EVAL0(__VA_ARGS__)))
#define EVAL2(...) EVAL1(EVAL1(EVAL1(__VA_ARGS__)))
#define EVAL3(...) EVAL2(EVAL2(EVAL2(__VA_ARGS__)))
#define EVAL4(...) EVAL3(EVAL3(EVAL3(__VA_ARGS__)))
#define EVAL(...)  EVAL4(EVAL4(EVAL4(__VA_ARGS__)))

#define MAP_END(...)
#define MAP_OUT

#define MAP_GET_END2() 0, MAP_END
#define MAP_GET_END1(...) MAP_GET_END2
#define MAP_GET_END(...) MAP_GET_END1

#define MAP_NEXT0(test, next, ...) next MAP_OUT
#define MAP_NEXT1(test, next) MAP_NEXT0(test, next, 0)
#define MAP_NEXT(test, next)  MAP_NEXT1(MAP_GET_END test, next)

#define MAP0(f, options, x, peek, ...) f(options, x) MAP_NEXT(peek, MAP1)(f, options, peek, __VA_ARGS__)
#define MAP1(f, options, x, peek, ...) f(options, x) MAP_NEXT(peek, MAP0)(f, options, peek, __VA_ARGS__)

#define MAP(f, options, ...) EVAL(MAP1(f, options, __VA_ARGS__, ()()(), ()()(), ()()(), 0))

// ---------- DEPAREN MACRO FUNTION --------------
// source: https://stackoverflow.com/a/62984543/3123852
// by: Nero [Actually a Magician]

#define DEPAREN(X) ESC(ISH X)
#define ISH(...) ISH __VA_ARGS__
#define ESC(...) ESC_(__VA_ARGS__)
#define ESC_(...) VAN ## __VA_ARGS__
#define VANISH

// ------- MAKE_BUILDER MACRO to generate struct builder --------
// by: Azeez Abass [Some Guy]

#define CALL(f, ...) f(__VA_ARGS__)

#define MAKE_SETTER(BUILDER_NAME, STRUCT_TYPE, PROP) \
    BUILDER_NAME& PROP(decltype(STRUCT_TYPE::PROP) value) { \
        m_obj.PROP = value; \
        return *this; \
    }

#define MAKE_SETTER_WITH_OPTS(OPTS, PROP)\
    CALL(MAKE_SETTER, DEPAREN(OPTS), PROP)


#define MAKE_BUILDER(BUILDER_NAME, STRUCT_TYPE, ...) \
    class BUILDER_NAME { \
      public: \
        BUILDER_NAME() {} \
        explicit BUILDER_NAME(const STRUCT_TYPE& obj) : m_obj(obj) {} \
        BUILDER_NAME& operator=(const STRUCT_TYPE& obj) { \
            m_obj = obj; \
            return *this; \
        } \
        operator STRUCT_TYPE() const { return m_obj; } \
        STRUCT_TYPE build() const { return m_obj; } \
        MAP(MAKE_SETTER_WITH_OPTS,(BUILDER_NAME, STRUCT_TYPE), __VA_ARGS__) \
      private: \
        STRUCT_TYPE m_obj; \
    };

#endif