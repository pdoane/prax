// A sortable table of users, as a React component with typed props.
import { useMemo, useState, type ReactNode } from "react";

export interface User {
    id: number;
    name: string;
    email: string;
    joined: Date;
}

type Column = keyof Pick<User, "name" | "email" | "joined">;

interface Props {
    users: readonly User[];
    empty?: ReactNode;
    onSelect?: (user: User) => void;
}

const headings: Record<Column, string> = { name: "Name", email: "Email", joined: "Joined" };

export function UserTable({ users, empty = <p>No users yet.</p>, onSelect }: Props) {
    const [column, setColumn] = useState<Column>("name");
    const [ascending, setAscending] = useState(true);

    const sorted = useMemo(() => {
        const copy = [...users];
        copy.sort((a, b) => {
            const order = a[column] < b[column] ? -1 : a[column] > b[column] ? 1 : 0;
            return ascending ? order : -order;
        });
        return copy;
    }, [users, column, ascending]);

    if (users.length === 0) return <>{empty}</>;

    return (
        <table className="users">
            <thead>
                <tr>
                    {(Object.keys(headings) as Column[]).map((key) => (
                        <th key={key} aria-sort={key === column ? (ascending ? "ascending" : "descending") : "none"}
                            onClick={() => (key === column ? setAscending(!ascending) : setColumn(key))}>
                            {headings[key]}
                        </th>
                    ))}
                </tr>
            </thead>
            <tbody>
                {sorted.map((user) => (
                    <tr key={user.id} onClick={() => onSelect?.(user)}>
                        <td>{user.name}</td>
                        <td><a href={`mailto:${user.email}`}>{user.email}</a></td>
                        <td>{user.joined.toLocaleDateString()}</td>
                    </tr>
                ))}
            </tbody>
        </table>
    );
}
