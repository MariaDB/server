/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#pragma once

#include <cassert>
#include <iterator>
#include <memory>

#include <db.h>

namespace ftcxx {

    class Slice {
    public:
        Slice()
            : _data(nullptr),
              _size(0)
        {}

        explicit Slice(size_t sz)
            : _buf(new char[sz], std::default_delete<char[]>()),
              _data(_buf.get()),
              _size(sz)
        {}

        Slice(const char *p, size_t sz)
            : _data(p),
              _size(sz)
        {}

        explicit Slice(const DBT &d)
            : _data(reinterpret_cast<char *>(d.data)),
              _size(d.size)
        {}

        explicit Slice(const std::string &str)
            : _data(str.c_str()),
              _size(str.size())
        {}

        Slice(const Slice &other)
            : _buf(other._buf),
              _data(other._data),
              _size(other._size)
        {}

        Slice& operator=(const Slice &other) {
            _buf = other._buf;
            _data = other._data;
            _size = other._size;
            return *this;
        }

        Slice(Slice&& other)
            : _buf(),
              _data(nullptr),
              _size(0)
        {
            std::swap(_buf, other._buf);
            std::swap(_data, other._data);
            std::swap(_size, other._size);
        }

        Slice& operator=(Slice&& other) {
            std::swap(_buf, other._buf);
            std::swap(_data, other._data);
            std::swap(_size, other._size);
            return *this;
        }

        template<typename T>
        static Slice slice_of(const T &v) {
            return Slice(reinterpret_cast<const char *>(&v), sizeof v);
        }

        template<typename T>
        T as() const {
            assert(size() == sizeof(T));
            const T *p = reinterpret_cast<const T *>(data());
            return *p;
        }

        const char *data() const { return _data; }

        char *mutable_data() const {
            assert(_buf);
            return _buf.get();
        }

        size_t size() const { return _size; }

        bool empty() const { return size() == 0; }

        char operator[](size_t n) const {
            assert(n < size());
            return _data[n];
        }

        char *begin() { return mutable_data(); }
        char *end() { return mutable_data() + size(); }
        char *rbegin() { return end(); }
        char *rend() { return begin(); }
        const char *begin() const { return data(); }
        const char *end() const { return data() + size(); }
        const char *rbegin() const { return end(); }
        const char *rend() const { return begin(); }
        const char *cbegin() const { return data(); }
        const char *cend() const { return data() + size(); }
        const char *crbegin() const { return end(); }
        const char *crend() const { return begin(); }

        Slice copy() const {
            Slice s(size());
            std::copy(begin(), end(), s.begin());
            return s;
        }

        Slice owned() const {
            if (_buf) {
                return *this;
            } else {
                return copy();
            }
        }

        DBT dbt() const {
            DBT d;
            d.data = const_cast<void *>(static_cast<const void *>(data()));
            d.size = size();
            d.ulen = size();
            d.flags = 0;
            return d;
        }

    private:
        std::shared_ptr<char> _buf;
        const char *_data;
        size_t _size;
    };

} // namespace ftcxx

namespace std {

    template<>
    class iterator_traits<ftcxx::Slice> {
        typedef typename std::iterator_traits<const char *>::difference_type difference_type;
        typedef typename std::iterator_traits<const char *>::value_type value_type;
        typedef typename std::iterator_traits<const char *>::pointer pointer;
        typedef typename std::iterator_traits<const char *>::reference reference;
        typedef typename std::iterator_traits<const char *>::iterator_category iterator_category;
    };

} // namespace std
