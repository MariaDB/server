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

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <sstream>
#include <vector>

#include "buffer.hpp"

class Item {
    const size_t _sz;

public:
    Item(size_t sz=0)
        : _sz(sz)
    {}

    operator std::string() const {
        std::stringstream ss;
        ss << "Item(" << _sz << ")";
        return ss.str();
    }

    bool operator==(const Item &other) const {
        return _sz == other._sz;
    }

    bool operator!=(const Item &other) const {
        return !(*this == other);
    }

    size_t serialized_size() const {
        return (sizeof _sz) + _sz;
    }

    void serialize(char *p) const {
        size_t *szp = reinterpret_cast<size_t *>(p);
        *szp = _sz;
    }

    static Item deserialize(const char *p) {
        const size_t *szp = reinterpret_cast<const size_t *>(p);
        return Item(*szp);
    }

    bool check_serialized(const char *p) {
        return deserialize(p) == *this;
    }
};

class SingleSizeGenerator {
    const size_t _sz;

public:
    SingleSizeGenerator(size_t sz)
        : _sz(sz)
    {}

    std::string name() const {
        std::stringstream ss;
        ss << "SingleSizeGenerator(" << _sz << ")";
        return ss.str();
    }

    Item next() {
        return Item(_sz);
    }
};

class RoundRobinGenerator {
    const std::vector<size_t> _szs;
    std::vector<size_t>::const_iterator _it;

public:
    RoundRobinGenerator(const std::vector<size_t> &szs)
        : _szs(szs),
          _it(_szs.begin())
    {}

    std::string name() const {
        std::stringstream ss;
        ss << "RoundRobinGenerator(";
        for (auto it = _szs.begin(); it != _szs.end(); ++it) {
            if (it != _szs.begin()) {
                ss << ", ";
            }
            ss << *it;
        }
        ss << ")";
        return ss.str();
    }

    Item next() {
        if (_it == _szs.end()) {
            _it = _szs.begin();
        }
        return Item(*(_it++));
    }
};

template<class Generator>
void test(Generator gen) {
    std::vector<Item> expected;
    std::vector<Item> received;

    const size_t N = 1000000;

    ftcxx::Buffer b;

    std::cout << gen.name() << ": ";

    for (size_t i = 0; i < N; ++i) {
        if (b.full()) {
            // drain
            while (b.more()) {
                Item it = Item::deserialize(b.current());
                received.push_back(it);
                b.advance(it.serialized_size());
            }
            b.clear();
        }

        // push
        Item it = gen.next();
        expected.push_back(it);
        char *p = b.alloc(it.serialized_size());
        it.serialize(p);
    }

    // drain one more time
    while (b.more()) {
        Item i = Item::deserialize(b.current());
        received.push_back(i);
        b.advance(i.serialized_size());
    }
    b.clear();

    if (expected.size() != received.size()) {
        std::cout << "fail" << std::endl;
        std::cerr << "expected.size() != received.size()" << std::endl;
        std::cerr << expected.size() << " != " << received.size() << std::endl;
        return;
    }

    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != received[i]) {
            std::cout << "fail" << std::endl;
            std::cerr << "expected[" << i << "] != received[" << i << "]" << std::endl;
            std::cerr << std::string(expected[i]) << " != " << std::string(received[i]) << std::endl;
            return;
        }
    }

    std::cout << "ok" << std::endl;
}

int main(void) {
    test(SingleSizeGenerator(1));
    test(SingleSizeGenerator(3));
    test(SingleSizeGenerator(32));
    test(SingleSizeGenerator(1<<11));
    test(SingleSizeGenerator(1<<12));
    test(SingleSizeGenerator((1<<12) - 1));
    test(SingleSizeGenerator((1<<12) + 1));
    test(SingleSizeGenerator(1<<20));

    test(RoundRobinGenerator({8, 16}));
    test(RoundRobinGenerator({8, 1<<12}));
    test(RoundRobinGenerator({8, (1<<12) - 1}));
    test(RoundRobinGenerator({8, (1<<12) + 1}));
    test(RoundRobinGenerator({8, (1<<12) - 1, (1<<12) + 1}));
    test(RoundRobinGenerator({8, (1<<20)}));
    test(RoundRobinGenerator({(1<<12) - 1, (1<<12) + 1}));
    test(RoundRobinGenerator({(1<<12)    , (1<<12) + 1}));
    test(RoundRobinGenerator({(1<<12) - 1, (1<<12)    }));
    test(RoundRobinGenerator({1<<12, 1<<20}));
    test(RoundRobinGenerator({1<<16, 1<<17}));

    return 0;
}
